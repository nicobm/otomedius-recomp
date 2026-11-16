"""Instruction decoding and function discovery for the Otomedius recompiler.

Discovery is a recursive descent over the PE code section, seeded with the
entry point, the export table, every direct branch target found on the way,
every code pointer found in the data sections and the classic MSVC function
prologue pattern.  Indirect jumps through a table (`jmp [reg*4+tbl]`) are
resolved by reading the table out of the image.
"""

import struct

import capstone
from capstone import x86 as cx

MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
MD.detail = True
MD.skipdata = False

# Instructions that end a basic block.
TERMINATORS = {
    "ret", "retf", "jmp", "ljmp", "iret", "iretd", "hlt", "ud2", "int3",
    "int", "into", "sysenter", "sysexit",
}
# Conditional jumps / loop family.
COND_JUMPS = {
    "ja", "jae", "jb", "jbe", "jc", "jcxz", "jecxz", "je", "jg", "jge",
    "jl", "jle", "jna", "jnae", "jnb", "jnbe", "jnc", "jne", "jng", "jnge",
    "jnl", "jnle", "jno", "jnp", "jns", "jnz", "jo", "jp", "jpe", "jpo",
    "js", "jz", "loop", "loope", "loopne",
}


class Insn:
    """Flat, picklable view of one decoded instruction."""

    __slots__ = ("addr", "size", "raw", "mnemonic", "opstr", "ops",
                 "groups", "eflags_w", "eflags_r")

    def __init__(self, insn):
        self.addr = insn.address
        self.size = insn.size
        self.raw = bytes(insn.bytes)
        self.mnemonic = insn.mnemonic
        self.opstr = insn.op_str
        self.groups = tuple(insn.groups)
        self.eflags_w = insn.eflags & 0xFFFF
        self.eflags_r = (insn.eflags >> 16) & 0xFFFF
        ops = []
        for o in insn.operands:
            if o.type == cx.X86_OP_REG:
                ops.append(("reg", insn.reg_name(o.reg), o.size, None))
            elif o.type == cx.X86_OP_IMM:
                ops.append(("imm", o.imm & 0xFFFFFFFF, o.size, None))
            elif o.type == cx.X86_OP_MEM:
                m = o.mem
                base = insn.reg_name(m.base) if m.base else None
                index = insn.reg_name(m.index) if m.index else None
                ops.append(("mem", m.disp & 0xFFFFFFFF, o.size, (base, index, m.scale)))
            else:
                ops.append(("err", 0, 0, None))
        self.ops = tuple(ops)

    @property
    def next(self):
        return self.addr + self.size

    def __repr__(self):
        return "<%08x %s %s>" % (self.addr, self.mnemonic, self.opstr)


def decode_at(image, addr, limit=1):
    n = 0
    for insn in MD.disasm(bytes(image[addr:addr + 16]), addr):
        yield Insn(insn)
        n += 1
        if n >= limit:
            break


class Program:
    def __init__(self, pe):
        self.pe = pe
        self.base = pe.image_base
        # Image indexed by absolute VA (the recompiled code uses 32-bit
        # addresses with the image base baked in, like the original loader).
        raw = pe.image()
        img = bytearray(self.base + len(raw))
        img[self.base:] = raw
        self.image = img
        self.text = [(self.base + s.va, self.base + s.va + min(s.vsize, s.rawsize))
                     for s in pe.sections if s.executable]
        self.readable = [(self.base + s.va, self.base + s.va + max(s.vsize, s.rawsize))
                         for s in pe.sections if s.readable]
        self.lo = min(a for a, _b in self.text) if self.text else 0
        self.hi = max(b for _a, b in self.text) if self.text else 0
        self.insns = {}
        self.funcs = {}          # va -> set of instruction addresses
        self.continuations = {}  # va -> function entry it resumes
        self.jt_targets = {}     # va of jmp insn -> tuple of targets
        self.bad = {}            # addr -> reason
        self.seeds = {}          # authoritative seeds (va -> description)
        self.prologue_seeds = set()  # heuristic seeds: traced last
        self.attempted = set()   # entries whose trace already ran
        self.code_ptrs = set()   # immediate operands that point into .text
        self.owner = {}          # instruction address -> function entry

    # ------------------------------------------------------------------ data
    def read32(self, addr):
        return struct.unpack_from("<I", self.image, addr)[0]

    def is_code(self, addr):
        return any(a <= addr < b for a, b in self.text)

    def is_readable(self, addr):
        return any(a <= addr < b for a, b in self.readable)

    # --------------------------------------------------------------- tracing
    def _decode(self, addr):
        i = self.insns.get(addr)
        if i is not None:
            return i
        if not self.is_code(addr):
            return None
        try:
            i = next(decode_at(self.image, addr, 1))
        except StopIteration:
            self.bad[addr] = "undecodable"
            return None
        self.insns[addr] = i
        return i

    def _jump_table(self, insn, entry=None):
        """Resolve `jmp dword ptr [reg*4 + table]` into a tuple of targets."""
        if insn.mnemonic not in ("jmp", "ljmp") or len(insn.ops) != 1:
            return None
        kind, disp, _scale, mem = insn.ops[0]
        if kind != "mem" or mem is None:
            return None
        _base, index, size = mem
        if index is None or size != 4:
            return None
        table = disp
        targets = []
        if not self.is_readable(table):
            return None
        # MSVC sometimes biases a table by one word (memcpy's
        # "jmp [eax*4+table]" really starts at table+4), so a leading non-code
        # word does not mean there is no table.  After that the entries must be
        # contiguous code pointers.
        for k in range(512):
            if not self.is_readable(table + k * 4):
                break
            t = self.read32(table + k * 4)
            if not self.is_code(t):
                break
            targets.append(t)
        if not targets:
            return None
        self.jt_targets[insn.addr] = tuple(targets)
        return tuple(targets)

    def trace_function(self, entry):
        """Follow control flow from `entry`, collecting instructions.

        Every address belongs to exactly one function: a path that reaches code
        already owned by another function stops there (that is a shared tail
        block or an inlined callee traced from its own entry).
        """
        if entry in self.funcs or entry in self.attempted:
            return
        self.attempted.add(entry)
        body = set()
        self.funcs[entry] = body
        work = [entry]
        while work:
            addr = work.pop()
            while True:
                if addr in body:
                    break
                owner = self.owner.get(addr)
                if owner is not None and owner != entry:
                    break
                insn = self._decode(addr)
                if insn is None:
                    break
                self.owner[addr] = entry
                body.add(addr)
                m = insn.mnemonic
                # Immediate operands that point into the code section are
                # usually function addresses (callbacks, vtables built on the
                # stack, ...).  They become discovery seeds in a second pass.
                for op in insn.ops:
                    if op[0] == "imm" and 0x400000 <= op[1] < 0x600000 and \
                            self.is_code(op[1]):
                        self.code_ptrs.add(op[1])
                if m == "call" and insn.ops:
                    kind, imm, _s, _m = insn.ops[0]
                    if kind == "imm" and self.is_code(imm):
                        self.seeds.setdefault(imm, "call from %08x" % addr)
                    self._continuation(insn.next, entry)
                    addr = insn.next
                    continue
                if m in ("ret", "retf", "iret", "iretd", "hlt", "ud2", "int3",
                         "int", "into", "sysenter"):
                    break
                if m == "jmp":
                    kind, imm, _s, _m = insn.ops[0]
                    if kind == "imm" and self.is_code(imm):
                        o = self.owner.get(imm)
                        if o is not None and o != entry:
                            break                     # tail call into a known function
                        work.append(imm)
                        break
                    tbl = self._jump_table(insn, entry)
                    if tbl:
                        for t in tbl:
                            work.append(t)
                        break
                    self.bad[addr] = "indirect jmp %s" % insn.opstr
                    break
                if m in COND_JUMPS:
                    kind, imm, _s, _m = insn.ops[0]
                    if kind == "imm" and self.is_code(imm):
                        work.append(imm)
                    addr = insn.next
                    continue
                addr = insn.next
        if not body:
            del self.funcs[entry]

    def _continuation(self, addr, entry):
        if addr not in self.insns and addr not in self.funcs:
            self.continuations[addr] = entry
            # make sure the following code is decoded
            if addr not in self.seeds:
                self.seeds[addr] = "return site of %08x" % entry

    # -------------------------------------------------------------- seeding
    def seed_all(self):
        self.seeds[self.base + self.pe.entry] = "entry point"
        for name, va in self.pe.exports().items():
            self.seeds[self.base + va] = "export %s" % name
        # code pointers stored in the data sections (C++ vtables, callbacks)
        for s in self.pe.sections:
            if s.executable or not s.rawsize:
                continue
            start = self.base + s.va
            blob = self.image[start:start + s.rawsize]
            for k in range(0, len(blob) - 3, 4):
                v = struct.unpack_from("<I", blob, k)[0]
                if self.is_code(v) and v not in self.seeds:
                    self.seeds[v] = "pointer in %s+%x" % (s.name, k)

    def discover(self):
        self.seed_all()

        # 1. authoritative seeds: entry point, exports, data pointers, call
        #    targets found while tracing.  Trace until nothing new appears.
        while True:
            pending = sorted(a for a in self.seeds
                             if a not in self.attempted and a not in self.continuations)
            if not pending:
                break
            for a in pending:
                self.trace_function(a)

        # 2. code pointers taken as immediate operands (callbacks, vtables
        #    built on the stack, ...)
        for a in sorted(self.code_ptrs):
            if a not in self.owner:
                self.trace_function(a)

        # 3. the prologue heuristic, only for code nothing else claimed.  It is
        #    the weakest seed: a false positive inside another function would
        #    steal its body, so it runs last.
        for a in sorted(self.prologue_seeds):
            if a not in self.owner:
                self.trace_function(a)

        # 4. every branch target and every call return site is a potential
        #    entry point, even when it is reached from another function (tail
        #    jumps, shared epilogues, callbacks).  Recompute until stable.
        while True:
            leaders = set()
            for a, i in self.insns.items():
                m = i.mnemonic
                if m in COND_JUMPS or m == "jmp":
                    if i.ops and i.ops[0][0] == "imm":
                        leaders.add(i.ops[0][1])
                    for t in self.jt_targets.get(a, ()):
                        leaders.add(t)
                if m == "call":
                    leaders.add(i.next)
                    if i.ops and i.ops[0][0] == "imm":
                        leaders.add(i.ops[0][1])
            fresh = [a for a in sorted(leaders) if a not in self.owner and self.is_code(a)]
            if not fresh:
                break
            for a in fresh:
                self.trace_function(a)

        # 5. data words that point at a real instruction start (jump tables
        #    whose pattern we did not recognise, vtables in .data, ...) are
        #    entry points too.  Only known instruction boundaries are accepted,
        #    so no block can start mid-instruction.
        self.extra_leaders = leaders
        covered = bytearray(self.base + len(self.image))
        for a, i in self.insns.items():
            covered[a:a + i.size] = b"\1" * i.size
        adopted = []
        for start, end in self.readable:
            for k in range(start, end - 3, 4):
                v = int.from_bytes(self.image[k:k + 4], "little")
                if not self.is_code(v):
                    continue
                if v in self.insns:
                    self.extra_leaders.add(v)
                elif not covered[v] and v not in self.attempted:
                    adopted.append(v)
        # Code that only a data word points at (an irregular jump table, a
        # chunk of memcpy, ...) is adopted as its own entry point.  This runs
        # last and never claims code another function already owns, so it can
        # only fill holes.
        for v in sorted(adopted):
            if v not in self.owner:
                self.trace_function(v)
        for v in adopted:
            if v in self.insns:
                self.extra_leaders.add(v)

    # ------------------------------------------------------------- statistics
    def coverage(self):
        total = sum(b - a for a, b in self.text)
        covered = sum(i.size for i in self.insns.values())
        return covered, total
