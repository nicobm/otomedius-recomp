"""x86 -> C emitter for the Otomedius recompiler.

Code model
----------
Every discovered function becomes `void fn_00XXXXXX(void)`.  Registers live in
a per-thread `cpu_t`, memory is the original 32-bit address space (`MEM`), and
control flow inside a function is a C `goto`.

`call`, `ret`, indirect jumps and tail jumps leave the function and come back
through the dispatcher in `x86rt.c`: it maps a 32-bit address to a
(function, continuation) pair, so a return address is a first class entry
point.  Native Win32 calls are inlined when the call target is an IAT slot.
"""

import re

from capstone import x86 as cx

import xdis

REG32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
REG16 = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di"]
REG8 = ["al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"]
XMM = ["xmm%d" % i for i in range(8)]
SEGS = ["es", "cs", "ss", "ds", "fs", "gs"]
CTYPE = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}

CC = {
    "o": "cpu->of", "no": "!cpu->of",
    "b": "cpu->cf", "c": "cpu->cf", "nae": "cpu->cf",
    "ae": "!cpu->cf", "nb": "!cpu->cf", "nc": "!cpu->cf",
    "e": "cpu->zf", "z": "cpu->zf",
    "ne": "!cpu->zf", "nz": "!cpu->zf",
    "be": "(cpu->cf || cpu->zf)", "na": "(cpu->cf || cpu->zf)",
    "a": "(!cpu->cf && !cpu->zf)", "nbe": "(!cpu->cf && !cpu->zf)",
    "s": "cpu->sf", "ns": "!cpu->sf",
    "p": "cpu->pf", "pe": "cpu->pf",
    "np": "!cpu->pf", "po": "!cpu->pf",
    "l": "(cpu->sf != cpu->of)", "nge": "(cpu->sf != cpu->of)",
    "ge": "cpu->sf == cpu->of", "nl": "cpu->sf == cpu->of",
    "le": "(cpu->zf || (cpu->sf != cpu->of))", "ng": "(cpu->zf || (cpu->sf != cpu->of))",
    "g": "(!cpu->zf && cpu->sf == cpu->of)", "nle": "(!cpu->zf && cpu->sf == cpu->of)",
}
JCC = {"j" + k: v for k, v in CC.items()}
SETCC = dict(CC)

ALU = {"add": "x86_add", "or": "x86_or", "adc": "x86_adc", "sbb": "x86_sbb",
       "and": "x86_and", "sub": "x86_sub", "xor": "x86_xor", "cmp": "x86_cmp"}
ARITH = set(ALU) | {"test"}
UNARY = {"inc": "x86_inc", "dec": "x86_dec", "neg": "x86_neg"}
SHIFTS = {"shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr"}
STRINGS = {"movs": 0, "stos": 1, "lods": 2, "cmps": 3, "scas": 4}
SSE_FP = {"addsd": "add", "subsd": "sub", "mulsd": "mul", "divsd": "div",
          "addss": "add", "subss": "sub", "mulss": "mul", "divss": "div",
          "addpd": "add", "subpd": "sub", "mulpd": "mul", "divpd": "div",
          "andpd": "and", "andnpd": "andn", "orpd": "or", "xorpd": "xor"}
SSE_CMP = {"comisd", "ucomisd", "comiss", "ucomiss"}


def c_u(v, size):
    return "0x%xu" % (v & ((1 << (size * 8)) - 1))


def st_index(op):
    if op[0] != "reg":
        return None
    if op[1] == "st":
        return 0
    m = re.match(r"st\((\d)\)", op[1])
    return int(m.group(1)) if m else None


def ia_addr(op):
    kind, disp, size, mem = op
    if kind != "mem" or mem is None:
        return None
    base, index, _scale = mem
    if base or index or size != 4:
        return None
    return disp


class Block:
    __slots__ = ("start", "insns", "label")

    def __init__(self, start, insns):
        self.start = start
        self.insns = insns
        self.label = "L_%08x" % start


class Func:
    def __init__(self, entry, prog):
        self.entry = entry
        self.addrs = sorted(a for a in prog.funcs.get(entry, ()))
        self.name = "fn_%08x" % entry
        self.blocks = []

    def split(self, prog):
        insns = [(a, prog.insns[a]) for a in self.addrs]
        index = {a: i for i, (a, _i) in enumerate(insns)}
        leaders = {self.entry}
        for a in getattr(prog, "extra_leaders", ()):
            if a in index:
                leaders.add(a)
        for a, i in insns:
            m = i.mnemonic
            nxt = a + i.size
            if m == "call" or m in xdis.COND_JUMPS or m == "jmp":
                if nxt in index:
                    leaders.add(nxt)
                t = i.ops[0] if i.ops else None
                if t is not None and t[0] == "imm" and t[1] in index:
                    leaders.add(t[1])
                for tt in prog.jt_targets.get(a, ()):
                    if tt in index:
                        leaders.add(tt)
        self.blocks = []
        leader_set = {a for a in leaders if a in index}
        for s in sorted(leader_set):
            body = []
            a = s
            while a in index:
                ai, ins = insns[index[a]]
                body.append((ai, ins))
                nxt = ai + ins.size
                if ins.mnemonic in ("call", "jmp", "ret", "retf", "iret", "iretd",
                                    "hlt", "ud2") or ins.mnemonic in xdis.COND_JUMPS:
                    break
                if nxt in leader_set:
                    break
                a = nxt
            self.blocks.append(Block(s, body))
        return self.blocks


class Emitter:
    def __init__(self, prog):
        self.prog = prog
        self.out = []
        self.unhandled = {}
        self.last_conts = []
        self.imp_slots = {}
        for dll, funcs in prog.pe.imports():
            for name, slot, _hint in funcs:
                self.imp_slots[prog.base + slot] = (dll, name)

    def w(self, line=""):
        self.out.append(line)

    # --------------------------------------------------------------- operands
    def ea(self, op):
        _k, disp, _size, mem = op
        base, index, scale = mem
        parts = []
        if base:
            parts.append("cpu->" + base)
        if index:
            if scale == 1:
                parts.append("(uint32_t)cpu->%s" % index)
            else:
                parts.append("((uint32_t)cpu->%s * %d)" % (index, scale))
        if disp or not parts:
            if parts:
                parts.append("0x%xu" % disp)
            else:
                return "0x%xu" % disp
        return "(uint32_t)(%s)" % " + ".join(parts)

    def seg(self, insn):
        """Segment override prefix, if any (scan only the prefix bytes)."""
        for p in insn.raw:
            if p in (0x64, 0x65, 0x26, 0x2E, 0x36, 0x3E):
                return {0x64: "fs", 0x65: "gs", 0x26: "es", 0x2E: "cs",
                        0x36: "ss", 0x3E: "ds"}[p]
            if p in (0x66, 0x67, 0xF0, 0xF2, 0xF3):
                continue
            break
        return None

    def read(self, insn, op, size=None):
        kind, val, osize, mem = op
        size = size or osize or 4
        if kind == "reg":
            n = val
            if n in REG32:
                return "(uint32_t)cpu->%s" % n
            if n in REG16:
                return "(uint32_t)(cpu->%s & 0xFFFFu)" % REG32[REG16.index(n)]
            if n in REG8:
                return "x86_r8(cpu, %d)" % REG8.index(n)
            if n in XMM:
                return "x86_xmm_get(cpu, %d, %d)" % (XMM.index(n), size)
            i = st_index(op)
            if i is not None:
                return "x86_st_get(cpu, %d)" % i
            if n in SEGS:
                return "(uint32_t)cpu->seg[%d]" % SEGS.index(n)
            raise ValueError("unhandled register %s" % n)
        if kind == "imm":
            return c_u(val, size)
        if kind == "mem":
            ea = self.ea(op)
            s = self.seg(insn)
            if s == "fs":
                return "x86_fs_read%d(cpu, %s)" % (size * 8, ea)
            if s == "gs":
                return "x86_gs_read%d(cpu, %s)" % (size * 8, ea)
            return "M%d(%s)" % (size * 8, ea)
        raise ValueError("bad operand %r" % (op,))

    def write(self, insn, op, expr, size=None):
        kind, val, osize, mem = op
        size = size or osize or 4
        if kind == "reg":
            n = val
            if n in REG32:
                if size == 4:
                    return "cpu->%s = (uint32_t)(%s);" % (n, expr)
                if size == 2:
                    return "cpu->%s = (cpu->%s & 0xFFFF0000u) | ((uint32_t)(%s) & 0xFFFFu);" % (n, n, expr)
                return "cpu->%s = (cpu->%s & 0xFFFFFF00u) | ((uint32_t)(%s) & 0xFFu);" % (n, n, expr)
            if n in REG16:
                r = REG32[REG16.index(n)]
                return "cpu->%s = (cpu->%s & 0xFFFF0000u) | ((uint32_t)(%s) & 0xFFFFu);" % (r, r, expr)
            if n in REG8:
                return "x86_r8_set(cpu, %d, (uint32_t)(%s));" % (REG8.index(n), expr)
            if n in XMM:
                return "x86_xmm_set(cpu, %d, %d, (uint64_t)(%s));" % (XMM.index(n), size, expr)
            if n in SEGS:
                return "cpu->seg[%d] = (uint16_t)(%s);" % (SEGS.index(n), expr)
            raise ValueError("unhandled register write %s" % n)
        if kind == "mem":
            ea = self.ea(op)
            s = self.seg(insn)
            if s in ("fs", "gs"):
                return "x86_%s_write%d(cpu, %s, (uint32_t)(%s));" % (s, size * 8, ea, expr)
            return "M%d(%s) = (%s)(%s);" % (size * 8, ea, CTYPE[size], expr)
        raise ValueError("cannot write %r" % (op,))

    # -------------------------------------------------------------- functions
    def emit_function(self, fn):
        prog = self.prog
        fn.split(prog)
        own = set(fn.addrs)
        cont_of = {}
        cid = 0
        for blk in fn.blocks:
            if blk.start == fn.entry:
                continue
            cid += 1
            cont_of[blk.start] = cid
        self.last_conts = [(a, c) for a, c in cont_of.items()]
        self.w("/* %s @ %08x: %d blocks */" % (fn.name, fn.entry, len(fn.blocks)))
        self.w("void %s(void)" % fn.name)
        self.w("{")
        self.w("  cpu_t *cpu = x86_cpu();")
        if cont_of:
            self.w("  switch (cpu->cont) {")
            for a, c in sorted(cont_of.items(), key=lambda kv: kv[1]):
                self.w("    case %d: cpu->cont = 0; goto L_%08x;" % (c, a))
            self.w("    default: cpu->cont = 0; break;")
            self.w("  }")
        first = fn.entry if any(b.start == fn.entry for b in fn.blocks) else \
            (fn.blocks[0].start if fn.blocks else fn.entry)
        self.w("  goto L_%08x;" % first)
        for blk in fn.blocks:
            self.w("%s: ;" % blk.label)
            for a, i in blk.insns:
                self.w("  /* %08x  %-8s %s */" % (a, i.mnemonic, i.opstr))
                for line in self.translate(fn, i, own):
                    self.w("    " + line)
        if fn.blocks:
            last = fn.blocks[-1].insns[-1][1]
            if last.mnemonic not in ("call", "jmp", "ret", "retf", "iret", "iretd",
                                     "hlt", "ud2") and last.mnemonic not in xdis.COND_JUMPS:
                # the body ends where another function starts: continue there
                self.w("  /* falls through into the next function */")
                self.w("  cpu->eip = 0x%xu; return;" % (last.addr + last.size))
        self.w("}")
        self.w()

    # -------------------------------------------------------------- translate
    def translate(self, fn, insn, own):
        prog = self.prog
        m = insn.mnemonic
        rep = 0
        while True:
            if m.startswith("lock "):
                m = m[5:]
            elif m.startswith("rep "):
                rep = 1
                m = m[4:]
            elif m.startswith("repe ") or m.startswith("repz "):
                rep = 1
                m = m[5:]
            elif m.startswith("repne ") or m.startswith("repnz "):
                rep = 2
                m = m[6:]
            else:
                break
        ops = insn.ops
        nxt = insn.next
        out = []

        def go(addr):
            if addr in own:
                out.append("goto L_%08x;" % addr)
            else:
                out.append("cpu->eip = 0x%xu; return;" % addr)

        def leave(addr):
            out.append("cpu->eip = 0x%xu; return;" % addr)

        if m in ALU:
            size = ops[0][2] or ops[1][2] or 4
            if m == "cmp":
                out.append("%s(cpu, %s, %s, %d);" % (ALU[m], self.read(insn, ops[0], size),
                                                    self.read(insn, ops[1], size), size))
            else:
                out.append(self.write(insn, ops[0], "%s(cpu, %s, %s, %d)" % (
                    ALU[m], self.read(insn, ops[0], size), self.read(insn, ops[1], size), size), size))
            return out

        if m == "test":
            size = ops[0][2] or 4
            out.append("x86_test(cpu, %s, %s, %d);" % (self.read(insn, ops[0], size),
                                                       self.read(insn, ops[1], size), size))
            return out

        if m == "mov":
            size = ops[0][2] or ops[1][2] or 4
            out.append(self.write(insn, ops[0], self.read(insn, ops[1], size), size))
            return out

        if m in ("movzx", "movsx"):
            ssz = ops[1][2]
            e = self.read(insn, ops[1], ssz)
            if m == "movsx":
                if ops[1][0] == "reg" and ops[1][1] in REG32:
                    # movsx r32, r/m32: same size, plain move
                    out.append(self.write(insn, ops[0], e, ops[0][2]))
                    return out
                e = "x86_signext(%s, %d)" % (e, ssz * 8)
            out.append(self.write(insn, ops[0], e, ops[0][2]))
            return out

        if m == "lea":
            out.append(self.write(insn, ops[0], self.ea(ops[1]), ops[0][2] or 4))
            return out

        if m == "xchg":
            size = ops[0][2] or 4
            out.append("{ uint32_t a_ = %s, b_ = %s;" % (self.read(insn, ops[0], size),
                                                         self.read(insn, ops[1], size)))
            out.append("  " + self.write(insn, ops[0], "b_", size))
            out.append("  " + self.write(insn, ops[1], "a_", size) + " }")
            return out

        if m in UNARY:
            size = ops[0][2] or 4
            out.append(self.write(insn, ops[0], "%s(cpu, %s, %d)" % (
                UNARY[m], self.read(insn, ops[0], size), size), size))
            return out
        if m == "not":
            size = ops[0][2] or 4
            out.append(self.write(insn, ops[0], "~%s" % self.read(insn, ops[0], size), size))
            return out

        if m in SHIFTS:
            size = ops[0][2] or 4
            cnt = self.read(insn, ops[1], 1) if len(ops) > 1 else "1u"
            out.append(self.write(insn, ops[0], "x86_%s(cpu, %s, %s, %d)" % (
                m, self.read(insn, ops[0], size), cnt, size), size))
            return out
        if m in ("shld", "shrd"):
            size = ops[0][2] or 4
            out.append(self.write(insn, ops[0], "x86_%s(cpu, %s, %s, %s, %d)" % (
                m, self.read(insn, ops[0], size), self.read(insn, ops[1], size),
                self.read(insn, ops[2], 1), size), size))
            return out

        if m in ("bt", "bts", "btr", "btc"):
            size = ops[0][2] or 4
            e = "x86_%s(cpu, %s, %s, %d)" % (m, self.read(insn, ops[0], size),
                                             self.read(insn, ops[1], size), size)
            if m == "bt":
                out.append(e + ";")
            else:
                out.append(self.write(insn, ops[0], e, size))
            return out

        if m in ("mul", "div", "idiv"):
            size = ops[0][2] or 4
            out.append("x86_%s(cpu, %s, %d);" % (m, self.read(insn, ops[0], size), size))
            return out
        if m == "imul":
            size = ops[0][2] or 4
            if len(ops) == 1:
                out.append("x86_imul1(cpu, %s, %d);" % (self.read(insn, ops[0], size), size))
            elif len(ops) == 2:
                out.append(self.write(insn, ops[0], "x86_imul2(cpu, %s, %s, %d)" % (
                    self.read(insn, ops[0], size), self.read(insn, ops[1], size), size), size))
            else:
                out.append(self.write(insn, ops[0], "x86_imul2(cpu, %s, %s, %d)" % (
                    self.read(insn, ops[1], size), self.read(insn, ops[2], size), size), size))
            return out

        if m == "push":
            op = ops[0]
            size = 4 if op[0] == "imm" else (op[2] or 4)
            if op[0] == "reg" and op[1] in SEGS:
                out.append("x86_push32(cpu, (uint32_t)cpu->seg[%d]);" % SEGS.index(op[1]))
            else:
                out.append("x86_push32(cpu, (uint32_t)(%s));" % self.read(insn, op, size))
            return out
        if m == "pop":
            size = ops[0][2] or 4
            if ops[0][0] == "reg" and ops[0][1] in SEGS:
                out.append("{ uint32_t v_; x86_pop32(cpu, &v_); cpu->seg[%d] = (uint16_t)v_; }"
                           % SEGS.index(ops[0][1]))
            elif ops[0][0] == "reg" and ops[0][1] in REG32 and size == 4:
                out.append("x86_pop32(cpu, &cpu->%s);" % ops[0][1])
            else:
                out.append("{ uint32_t v_; x86_pop32(cpu, &v_);")
                out.append("  " + self.write(insn, ops[0], "v_", size) + " }")
            return out
        if m in ("pushad", "pushal"):
            out.append("x86_pushad(cpu);")
            return out
        if m in ("popad", "popal"):
            out.append("x86_popad(cpu);")
            return out
        if m in ("pushfd", "pushf"):
            out.append("x86_pushfd(cpu);")
            return out
        if m in ("popfd", "popf"):
            out.append("x86_popfd(cpu);")
            return out
        if m == "leave":
            out.append("x86_leave(cpu);")
            return out
        if m == "enter":
            out.append("x86_enter_frame(cpu, %s, %s);" % (ops[0][1], ops[1][1]))
            return out

        if m == "call":
            tgt = ops[0]
            if tgt[0] == "imm":
                out.append("x86_push32(cpu, 0x%xu);" % nxt)
                leave(tgt[1])
            elif tgt[0] == "mem" and ia_addr(tgt) in self.imp_slots:
                out.append("x86_push32(cpu, 0x%xu);" % nxt)
                out.append("x86_native_call(0x%xu);" % ia_addr(tgt))
                go(nxt)
            elif tgt[0] == "mem":
                out.append("{ uint32_t ia_ = M32(%s);" % self.ea(tgt))
                out.append("  x86_push32(cpu, 0x%xu);" % nxt)
                out.append("  x86_jump(cpu, ia_); return; }")
            else:
                out.append("{ uint32_t ia_ = (uint32_t)(%s);" % self.read(insn, tgt, 4))
                out.append("  x86_push32(cpu, 0x%xu);" % nxt)
                out.append("  x86_jump(cpu, ia_); return; }")
            return out

        if m in ("ret", "retf"):
            if m == "retf":
                out.append("x86_unimpl(cpu, 0x%xu, \"retf\");" % insn.addr)
                out.append("cpu->eip = 0; return;")
                return out
            if len(ops) > 0:
                out.append("{ uint32_t ra_ = x86_peek32(cpu); cpu->esp += 4 + %d; "
                           "cpu->eip = ra_; return; }" % ops[0][1])
            else:
                out.append("cpu->eip = x86_ret_from(cpu, 0x%xu); return;" % insn.addr)
            return out

        if m == "jmp":
            tbl = prog.jt_targets.get(insn.addr)
            if tbl:
                out.append("{ uint32_t t_ = (uint32_t)(%s);" % self.jt_expr(insn))
                out.append("  switch (t_) {")
                for t in sorted(set(tbl)):
                    if t in own:
                        out.append("    case 0x%xu: goto L_%08x;" % (t, t))
                out.append("    default: cpu->eip = t_; return;")
                out.append("  } }")
                return out
            if ops[0][0] == "imm":
                go(ops[0][1])
            elif ops[0][0] == "mem" and ia_addr(ops[0]) in self.imp_slots:
                # an import thunk: jmp [IAT] == call the native function the
                # way its stdcall contract expects, then return to our caller
                out.append("x86_native_tail(0x%xu); return;" % ia_addr(ops[0]))
            elif ops[0][0] == "mem":
                out.append("{ uint32_t ia_ = M32(%s); x86_jump(cpu, ia_); return; }" % self.ea(ops[0]))
            else:
                out.append("{ uint32_t ia_ = (uint32_t)(%s); x86_jump(cpu, ia_); return; }"
                           % self.read(insn, ops[0], 4))
            return out

        if m in JCC:
            t = ops[0][1]
            out.append("if (%s) {" % JCC[m])
            if t in own:
                out.append("  goto L_%08x;" % t)
            else:
                out.append("  cpu->eip = 0x%xu; return;" % t)
            out.append("}")
            go(nxt)
            return out

        if m in ("loop", "loope", "loopne"):
            t = ops[0][1]
            out.append("cpu->ecx--;")
            cond = {"loop": "cpu->ecx != 0",
                    "loope": "cpu->ecx != 0 && cpu->zf",
                    "loopne": "cpu->ecx != 0 && !cpu->zf"}[m]
            out.append("if (%s) {" % cond)
            if t in own:
                out.append("  goto L_%08x;" % t)
            else:
                out.append("  cpu->eip = 0x%xu; return;" % t)
            out.append("}")
            go(nxt)
            return out

        if m.startswith("set") and m[3:] in SETCC:
            out.append(self.write(insn, ops[0], "(%s) ? 1u : 0u" % SETCC[m[3:]], 1))
            return out

        if m.startswith("cmov") and m[4:] in SETCC:
            size = ops[0][2] or 4
            out.append("if (%s) { %s }" % (SETCC[m[4:]],
                                           self.write(insn, ops[0], self.read(insn, ops[1], size), size)))
            return out

        if m == "cdq":
            out.append("cpu->edx = (cpu->eax & 0x80000000u) ? 0xFFFFFFFFu : 0u;")
            return out
        if m == "cwde":
            out.append("cpu->eax = (uint32_t)(int32_t)(int16_t)(cpu->eax & 0xFFFFu);")
            return out
        if m == "cbw":
            out.append("cpu->eax = (cpu->eax & 0xFFFF0000u) | (uint32_t)((cpu->eax & 0x8000u) ? 0xFFFFu : 0u);")
            return out
        if m == "cwd":
            out.append("cpu->edx = (cpu->edx & 0xFFFF0000u) | (uint32_t)((cpu->eax & 0x8000u) ? 0xFFFFu : 0u);")
            return out

        if m in ("nop", "wait", "fwait", "fnclex", "cli", "sti"):
            return out
        if m == "cld":
            out.append("cpu->df = 0;")
            return out
        if m == "std":
            out.append("cpu->df = 1;")
            return out
        if m == "clc":
            out.append("cpu->cf = 0;")
            return out
        if m == "stc":
            out.append("cpu->cf = 1;")
            return out
        if m == "cmc":
            out.append("cpu->cf ^= 1;")
            return out
        if m in ("sahf", "lahf"):
            out.append("x86_%s(cpu);" % m)
            return out
        if m == "cpuid":
            out.append("x86_cpuid(cpu);")
            return out
        if m == "rdtsc":
            out.append("x86_rdtsc(cpu);")
            return out
        if m in ("xlatb", "xlat"):
            out.append("cpu->eax = (cpu->eax & 0xFFFFFF00u) | M8(cpu->ebx + (cpu->eax & 0xFFu));")
            return out

        if m.rstrip("bwd") in STRINGS and not any(
                o[0] == "imm" or (o[0] == "reg" and o[1].startswith("xmm")) for o in ops):
            base = m.rstrip("bwd")
            opcode = insn.raw[-1]
            if opcode & 1:
                size = 2 if 0x66 in insn.raw[:-1] else 4
            else:
                size = 1
            if rep == 0:
                for p in insn.raw[:3]:
                    if p == 0xF3:
                        rep = 1
                    elif p == 0xF2:
                        rep = 2
            out.append("x86_string(cpu, %d, %d, %d);" % (STRINGS[base], size, rep))
            return out

        if m.startswith("f"):
            line = self.translate_x87(insn)
            if line is not None:
                return line

        if m in SSE_FP or m in SSE_CMP or m == "movsd" or m == "movss" or m.startswith(
                ("movap", "movdq", "movup", "movlp", "movhp", "pshuf", "psrl", "psll",
                 "pextr", "pinsr", "pmovmsk", "punpck", "unpck", "shuf", "cvt", "sqrt",
                 "stmxcsr", "ldmxcsr", "pxor", "por", "pand", "pcmpeq", "psub", "padd",
                 "pmax", "pmin", "pmul", "movd", "movq", "cmpeq", "cmplt")):
            line = self.translate_sse(insn)
            if line is not None:
                return line

        self.unhandled[m] = self.unhandled.get(m, 0) + 1
        out.append("x86_unimpl(cpu, 0x%xu, \"%s %s\");" % (insn.addr, m, insn.opstr.replace('"', "'")))
        return out

    def write_lvalue(self, insn, op, size):
        """Addressable lvalue for pop: registers only in practice."""
        kind, val, osize, mem = op
        if kind == "reg" and val in REG32 and size == 4:
            return "(uint32_t *)&cpu->" + val
        raise ValueError("pop to unsupported operand %r" % (op,))

    def jt_expr(self, insn):
        op = insn.ops[0]
        kind, disp, _size, mem = op
        base, index, scale = mem
        parts = []
        if base:
            parts.append("cpu->" + base)
        if index:
            parts.append("((uint32_t)cpu->%s * %d)" % (index, scale))
        parts.append("0x%xu" % disp)
        return "M32(%s)" % " + ".join(parts)

    # -------------------------------------------------------------- x87
    def translate_x87(self, insn):
        m = insn.mnemonic
        ops = insn.ops
        memop = ops[0] if ops and ops[0][0] == "mem" else None

        if m in ("fld", "flds", "fldl"):
            if memop:
                return ["x86_fld_mem(cpu, %s, %d);" % (self.ea(memop), memop[2] * 8)]
            i = st_index(ops[0])
            return ["x86_fld_st(cpu, %d);" % (i if i is not None else 0)]
        if m in ("fld1", "fldz", "fldpi", "fldlg2", "fldln2", "fldl2e"):
            return ["x86_fld_const(cpu, %d);" % {"fldz": 0, "fld1": 1, "fldpi": 2,
                                                 "fldlg2": 3, "fldln2": 4, "fldl2e": 5}[m]]
        if m in ("fild", "fildl", "fildll"):
            if memop:
                return ["x86_fild(cpu, %s, %d);" % (self.ea(memop), memop[2] * 8)]
            return None
        if m in ("fst", "fstp", "fsts", "fstps", "fstl", "fstpl", "fstpt"):
            pop = m in ("fstp", "fstps", "fstpl", "fstpt")
            if memop:
                return ["x86_fst_mem(cpu, %s, %d, %d);" % (self.ea(memop), memop[2] * 8, int(pop))]
            i = st_index(ops[0])
            return ["x86_fst_st(cpu, %d, %d);" % (i if i is not None else 0, int(pop))]
        if m in ("fistp", "fistpl", "fistpll", "fisttp", "fisttpl"):
            if memop:
                return ["x86_fist_mem(cpu, %s, %d, 1);" % (self.ea(memop), memop[2] * 8)]
            return None
        if m in ("fist", "fistl"):
            if memop:
                return ["x86_fist_mem(cpu, %s, %d, 0);" % (self.ea(memop), memop[2] * 8)]
            return None
        if m == "fxch":
            # capstone exposes `fxch st(i)` as two operands, (st(0), st(i));
            # older decoders give only st(i).  The exchange index is the
            # operand that is not st(0), never ops[0] (which is always st(0)).
            i = st_index(ops[1]) if len(ops) > 1 else None
            if i is None and ops:
                i = st_index(ops[0])
            if i is None:
                i = 1
            return ["x86_fxch(cpu, %d);" % i]
        if m in ("fadd", "faddp", "fsub", "fsubp", "fsubr", "fsubrp", "fmul", "fmulp",
                 "fdiv", "fdivp", "fdivr", "fdivrp", "fiadd", "fisub", "fisubr", "fimul",
                 "fidiv", "fidivr"):
            return [self.fpu_arith(insn)]
        if m in ("fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp", "ftst"):
            if memop:
                return ["x86_fpu_cmp_mem(cpu, %s, %d, %d);" % (
                    self.ea(memop), memop[2] * 8, int(m in ("fcomp",)))]
            if not ops:
                return ["x86_fpu_cmp(cpu, 1, %d);" % (2 if m.endswith("pp") else int(m in ("fcomp", "fucomp")))]
            i = st_index(ops[0])
            return ["x86_fpu_cmp(cpu, %d, %d);" % (i if i is not None else 0,
                                                   2 if m.endswith("pp") else int(m in ("fcomp", "fucomp")))]
        simple = {"fchs": "x86_fchs", "fabs": "x86_fabs", "fsqrt": "x86_fsqrt",
                  "fsin": "x86_fsin", "fcos": "x86_fcos", "fptan": "x86_fptan",
                  "fpatan": "x86_fpatan", "fyl2x": "x86_fyl2x", "f2xm1": "x86_f2xm1",
                  "frndint": "x86_frndint", "fscale": "x86_fscale", "fprem": "x86_fprem",
                  "fprem1": "x86_fprem", "fxam": "x86_fxam", "fxtract": "x86_fxtract"}
        if m in simple:
            return ["%s(cpu);" % simple[m]]
        if m in ("fnstsw", "fstsw"):
            if memop:
                return ["M16(%s) = (uint16_t)x86_fpu_sw(cpu);" % self.ea(memop)]
            return ["cpu->eax = (cpu->eax & 0xFFFF0000u) | (x86_fpu_sw(cpu) & 0xFFFFu);"]
        if m in ("fnstcw", "fstcw"):
            if memop:
                return ["M16(%s) = cpu->fpu_cw;" % self.ea(memop)]
            return None
        if m == "fldcw":
            if memop:
                return ["x86_fldcw(cpu, M16(%s));" % self.ea(memop)]
            return None
        if m in ("fnstenv", "fstenv"):
            if memop:
                return ["x86_fstenv(cpu, %s);" % self.ea(memop)]
            return None
        if m == "fldenv":
            if memop:
                return ["x86_fldenv(cpu, %s);" % self.ea(memop)]
            return None
        if m in ("fnsave", "fsave"):
            if memop:
                return ["x86_fsave(cpu, %s);" % self.ea(memop)]
            return None
        if m == "frstor":
            if memop:
                return ["x86_frstor(cpu, %s);" % self.ea(memop)]
            return None
        return None

    def fpu_arith(self, insn):
        m = insn.mnemonic
        ops = insn.ops
        pop = m.endswith("p") and m not in ("fiadd", "fisub", "fisubr", "fimul", "fidiv", "fidivr")
        base = m[:-1] if pop else m
        rev = base.endswith("r")
        if rev:
            base = base[:-1]
        memop = ops[0] if ops and ops[0][0] == "mem" else None
        opid = {"fadd": 0, "fiadd": 0, "fsub": 1, "fisub": 1, "fsubr": 1, "fisubr": 1,
                "fmul": 2, "fimul": 2, "fdiv": 3, "fidiv": 3, "fdivr": 3, "fidivr": 3}[base]
        if memop:
            return "x86_fpu_arith_mem(cpu, %d, %s, %d, %d, %d);" % (
                opid, self.ea(memop), memop[2] * 8, int(rev), int(pop))
        # pop forms (`faddp st(1)` and the implicit `faddp`) use st(1) as the
        # destination and st(0) as the popped operand when no operand is
        # decoded explicitly.
        a = st_index(ops[0]) if ops else (1 if pop else 0)
        if a is None:
            a = 1 if pop else 0
        b = st_index(ops[1]) if len(ops) > 1 else 0
        if b is None:
            b = 0
        return "x86_fpu_arith(cpu, %d, %d, %d, %d, %d);" % (opid, a, b, int(rev), int(pop))

    # -------------------------------------------------------------- SSE
    def sse_read(self, insn, op, size):
        if op[0] == "reg":
            if op[1].startswith("xmm"):
                return "x86_xmm_get(cpu, %d, %d)" % (XMM.index(op[1]), size)
            return self.read(insn, op, size)
        if op[0] == "mem":
            return "x86_sse_load(cpu, %s, %d)" % (self.ea(op), size)
        return self.read(insn, op, size)

    def sse_dst(self, insn, op, expr, size):
        if op[0] == "reg" and op[1].startswith("xmm"):
            return "x86_xmm_set(cpu, %d, %d, (uint64_t)(%s));" % (XMM.index(op[1]), size, expr)
        if op[0] == "mem":
            return "M%d(%s) = (%s)(%s);" % (size * 8, self.ea(op), CTYPE[size], expr)
        return self.write(insn, op, expr, size)

    def translate_sse(self, insn):
        m = insn.mnemonic
        ops = insn.ops
        if m in ("movsd", "movss", "movapd", "movaps", "movdqa", "movdqu", "movupd",
                 "movups", "movlpd", "movlps", "movhpd", "movhps"):
            if m in ("movsd", "movss"):
                size = 2 if m == "movsd" else 1
            elif m in ("movlpd", "movlps", "movhpd", "movhps"):
                size = 2
            else:
                size = 4
            dst, src = ops[0], ops[1]
            if m in ("movlpd", "movlps", "movhpd", "movhps"):
                high = 1 if m.startswith("movh") else 0
                if dst[0] == "mem":
                    return ["M64(%s) = x86_xmm_get64(cpu, %d, %d);" % (
                        self.ea(dst), XMM.index(src[1]), high)]
                return ["x86_xmm_set64(cpu, %d, %d, x86_sse_load(cpu, %s, 2));" % (
                    XMM.index(dst[1]), high, self.ea(src))]
            if dst[0] == "mem":
                return ["M%d(%s) = (%s)(%s);" % (size * 8, self.ea(dst), CTYPE[size],
                                                 self.sse_read(insn, src, size))]
            if src[0] == "mem":
                return ["x86_sse_store_xmm(cpu, %d, %s, %d);" % (XMM.index(dst[1]),
                                                                 self.ea(src), size)]
            return ["x86_sse_copy(cpu, %d, %d, %d);" % (XMM.index(dst[1]), XMM.index(src[1]), size)]
        if m in SSE_FP:
            size = 1 if m.endswith("ss") else 2
            dst, src = ops[0], ops[1]
            if not (dst[0] == "reg" and dst[1].startswith("xmm")):
                return None
            dv = "x86_xmm_get(cpu, %d, %d)" % (XMM.index(dst[1]), size)
            sv = self.sse_read(insn, src, size)
            return ["x86_sse_setop(cpu, %d, %d, %s, %s, %d);" % (
                XMM.index(dst[1]), size, dv, sv,
                {"add": 0, "sub": 1, "mul": 2, "div": 3, "and": 4, "andn": 5,
                 "or": 6, "xor": 7}[SSE_FP[m]])]
        if m in SSE_CMP:
            return ["x86_sse_comis(cpu, x86_xmm_get(cpu, %d, 2), %s);" % (
                XMM.index(ops[0][1]), self.sse_read(insn, ops[1], 2))]
        if m in ("pxor", "por", "pand", "pandn", "pcmpeqd", "pcmpeqb", "pcmpeqw"):
            return ["x86_sse_intop(cpu, %d, %s, %d);" % (
                XMM.index(ops[0][1]), self.sse_read(insn, ops[1], 4),
                {"pxor": 0, "por": 1, "pand": 2, "pandn": 3, "pcmpeqd": 4,
                 "pcmpeqb": 5, "pcmpeqw": 6}[m])]
        if m == "pshufd":
            return ["x86_pshufd(cpu, %d, %d, %s);" % (XMM.index(ops[0][1]), ops[2][1],
                                                      self.sse_read(insn, ops[1], 4))]
        if m in ("psrlq", "psllq", "psrld", "pslld", "psraw", "psrad", "psrlw", "psllw"):
            o = ops[1]
            if o[0] == "imm":
                cnt = "%d" % o[1]
            elif o[0] == "mem":
                cnt = "(int)x86_sse_load(cpu, %s, 2)" % self.ea(o)
            else:
                cnt = "(int)x86_xmm_lo32(cpu, %d)" % XMM.index(o[1])
            return ["x86_%s(cpu, %d, %s);" % (m, XMM.index(ops[0][1]), cnt)]
        if m == "pmovmskb":
            return ["cpu->eax = (cpu->eax & 0xFFFF0000u) | x86_pmovmskb(cpu, %d);" % XMM.index(ops[1][1])]
        if m == "pextrw":
            return ["cpu->eax = (cpu->eax & 0xFFFF0000u) | x86_pextrw(cpu, %d, %d);" % (
                XMM.index(ops[1][1]), ops[2][1])]
        if m == "pinsrw":
            return ["x86_pinsrw(cpu, %d, %d, %s);" % (XMM.index(ops[0][1]), ops[2][1],
                                                      self.sse_read(insn, ops[1], 2))]
        if m in ("unpcklpd", "unpckhpd", "punpckldq", "punpcklqdq", "punpckhdq", "punpckhqdq"):
            return ["x86_sse_unpck(cpu, %d, %s, %d, %d);" % (
                XMM.index(ops[0][1]), self.sse_read(insn, ops[1], 4),
                int("h" in m[:7]), {"unpcklpd": 2, "unpckhpd": 2}.get(m, 1))]
        if m == "shufpd":
            return ["x86_shufpd(cpu, %d, %s, %d);" % (XMM.index(ops[0][1]),
                                                      self.sse_read(insn, ops[1], 2), ops[2][1])]
        if m == "stmxcsr":
            return ["M32(%s) = cpu->mxcsr;" % self.ea(ops[0])]
        if m == "ldmxcsr":
            return ["cpu->mxcsr = M32(%s);" % self.ea(ops[0])]
        if m == "movd":
            dst, src = ops[0], ops[1]
            if dst[0] == "reg" and dst[1].startswith("xmm"):
                return ["x86_xmm_zero(cpu, %d); x86_xmm_set_lo32(cpu, %d, (uint32_t)(%s));" % (
                    XMM.index(dst[1]), XMM.index(dst[1]), self.read(insn, src, 4))]
            return [self.write(insn, dst, "x86_xmm_lo32(cpu, %d)" % XMM.index(src[1]), 4)]
        if m == "movq":
            dst, src = ops[0], ops[1]
            if dst[0] == "reg" and dst[1].startswith("xmm"):
                if src[0] == "reg" and src[1].startswith("xmm"):
                    return ["x86_sse_copy(cpu, %d, %d, 2);" % (XMM.index(dst[1]), XMM.index(src[1]))]
                return ["x86_xmm_set64(cpu, %d, 0, x86_sse_load(cpu, %s, 2)); x86_xmm_set64(cpu, %d, 1, 0);" % (
                    XMM.index(dst[1]), self.ea(src), XMM.index(dst[1]))]
            if src[0] == "reg" and src[1].startswith("xmm"):
                return ["M64(%s) = x86_xmm_get64(cpu, %d, 0);" % (self.ea(dst), XMM.index(src[1]))]
            return None
        if m in ("cvtsi2sd", "cvtsi2ss"):
            return ["x86_sse_setop(cpu, %d, 2, 0, 0, 0); x86_xmm_set(cpu, %d, 2, (uint64_t)x86_i2d(%s));" % (
                XMM.index(ops[0][1]), XMM.index(ops[0][1]), self.read(insn, ops[1], 4))]
        if m in ("cvttsd2si", "cvtsd2si"):
            reg = ops[0][1]
            if reg in REG32:
                return ["cpu->%s = (uint32_t)x86_d2i(%s);" % (
                    reg, self.sse_read(insn, ops[1], 2))]
            return None
        if m == "sqrtsd":
            return ["x86_xmm_set(cpu, %d, 2, (uint64_t)x86_sqrt(x86_xmm_get(cpu, %d, 2)));" % (
                XMM.index(ops[0][1]), XMM.index(ops[1][1]))]
        if m == "cvtdq2pd":
            return ["x86_xmm_set(cpu, %d, 2, (uint64_t)(double)(int32_t)x86_xmm_lo32(cpu, %d));" % (
                XMM.index(ops[0][1]), XMM.index(ops[0][1]))]
        if m in ("psubd", "paddd", "psubq", "paddq"):
            return ["x86_xmm_set_lo32(cpu, %d, x86_xmm_lo32(cpu, %d) %s x86_xmm_lo32(cpu, %d));" % (
                XMM.index(ops[0][1]), XMM.index(ops[0][1]),
                "-" if m.startswith("psub") else "+", XMM.index(ops[1][1]))]
        if m in ("pmaxsw", "pminsw", "pmulhw", "pmaddwd"):
            return ["x86_%s(cpu, %d, %s);" % (m, XMM.index(ops[0][1]),
                                              self.sse_read(insn, ops[1], 4))]
        return None


def tgt_of(insn):
    if insn.ops and insn.ops[0][0] == "imm":
        return insn.ops[0][1]
    return None
