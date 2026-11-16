"""Minimal PE32 reader used by the Otomedius static recompiler.

Only what the recompiler and the runtime need: section map, import and export
directories, base relocations, resources (version info) and the debug
directory.  No third-party dependency.
"""

import struct


class PEError(Exception):
    pass


class Section:
    __slots__ = ("name", "va", "vsize", "raw", "rawsize", "characteristics")

    def __init__(self, name, va, vsize, raw, rawsize, chars):
        self.name = name
        self.va = va
        self.vsize = vsize
        self.raw = raw
        self.rawsize = rawsize
        self.characteristics = chars

    @property
    def end(self):
        return self.va + max(self.vsize, self.rawsize)

    @property
    def readable(self):
        return bool(self.characteristics & 0x40000000)

    @property
    def writeable(self):
        return bool(self.characteristics & 0x80000000)

    @property
    def executable(self):
        return bool(self.characteristics & 0x20000000)

    def __repr__(self):
        return "<Section %s va=%08x size=%x>" % (self.name, self.va, self.vsize)


class PE:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:2] != b"MZ":
            raise PEError("not an MZ executable")
        lfanew = struct.unpack_from("<I", d, 0x3C)[0]
        if d[lfanew:lfanew + 4] != b"PE\0\0":
            raise PEError("missing PE signature")
        coff = lfanew + 4
        (machine, nsec, timestamp, symptr, nsym, optsize,
         chars) = struct.unpack_from("<HHIIIHH", d, coff)
        if machine != 0x14C:
            raise PEError("not an i386 image (machine %#x)" % machine)
        opt = coff + 20
        magic = struct.unpack_from("<H", d, opt)[0]
        if magic != 0x10B:
            raise PEError("not a PE32 image (magic %#x)" % magic)
        self.machine = machine
        self.timestamp = timestamp
        self.characteristics = chars
        self.entry = struct.unpack_from("<I", d, opt + 16)[0]
        self.image_base = struct.unpack_from("<I", d, opt + 28)[0]
        self.section_align = struct.unpack_from("<I", d, opt + 32)[0]
        self.file_align = struct.unpack_from("<I", d, opt + 36)[0]
        self.size_of_image = struct.unpack_from("<I", d, opt + 56)[0]
        self.size_of_headers = struct.unpack_from("<I", d, opt + 60)[0]
        self.subsystem = struct.unpack_from("<H", d, opt + 68)[0]
        self.dll_chars = struct.unpack_from("<H", d, opt + 70)[0]
        self.stack_reserve = struct.unpack_from("<I", d, opt + 72)[0]
        self.ndirs = struct.unpack_from("<I", d, opt + 92)[0]
        self.dirs = []
        for i in range(min(self.ndirs, 16)):
            rva, size = struct.unpack_from("<II", d, opt + 96 + i * 8)
            self.dirs.append((rva, size))
        so = opt + optsize
        self.sections = []
        for i in range(nsec):
            o = so + i * 40
            name = d[o:o + 8].rstrip(b"\0").decode("latin-1")
            vsize, va, rawsize, raw = struct.unpack_from("<IIII", d, o + 8)
            schars = struct.unpack_from("<I", d, o + 36)[0]
            self.sections.append(Section(name, va, vsize, raw, rawsize, schars))
        self.sections.sort(key=lambda s: s.va)
        self._overlay = self._find_overlay()

    # ------------------------------------------------------------------ utils
    def _find_overlay(self):
        end = self.size_of_headers
        for s in self.sections:
            if s.rawsize:
                end = max(end, s.raw + s.rawsize)
        if end < len(self.data):
            return self.data[end:]
        return b""

    @property
    def overlay(self):
        """Data appended after the last section (Konami packs its loader here)."""
        return self._overlay

    def section_of(self, rva):
        for s in self.sections:
            if s.va <= rva < s.end:
                return s
        return None

    def rva_to_off(self, rva):
        s = self.section_of(rva)
        if s is None:
            if rva < self.size_of_headers:
                return rva
            return None
        delta = rva - s.va
        if delta >= s.rawsize:
            return None
        return s.raw + delta

    def read(self, rva, size):
        off = self.rva_to_off(rva)
        if off is None:
            raise PEError("rva %#x is not backed by file data" % rva)
        return self.data[off:off + size]

    def u32(self, rva):
        return struct.unpack_from("<I", self.read(rva, 4))[0]

    def cstr(self, rva):
        off = self.rva_to_off(rva)
        if off is None:
            return ""
        end = self.data.index(b"\0", off)
        return self.data[off:end].decode("latin-1")

    def image(self):
        """Memory-mapped image (sections placed at their RVAs, zero filled)."""
        img = bytearray(max(self.size_of_image, self.size_of_headers))
        img[:self.size_of_headers] = self.data[:self.size_of_headers]
        for s in self.sections:
            n = min(s.rawsize, len(self.data) - s.raw)
            if n > 0:
                img[s.va:s.va + n] = self.data[s.raw:s.raw + n]
        return bytes(img)

    # ---------------------------------------------------------------- imports
    def imports(self):
        """[(dll_lower, [(name_or_ordinal, iat_rva, hint)])]"""
        if len(self.dirs) < 2:
            return []
        rva, size = self.dirs[1]
        if not rva:
            return []
        out = []
        off = self.rva_to_off(rva)
        while True:
            ilt, _ts, _fc, name_rva, iat = struct.unpack_from("<IIIII", self.data, off)
            if not (ilt or name_rva or iat):
                break
            dll = self.cstr(name_rva).lower()
            funcs = []
            t = self.rva_to_off(ilt or iat)
            k = 0
            while t is not None:
                v = struct.unpack_from("<I", self.data, t)[0]
                if v == 0:
                    break
                if v & 0x80000000:
                    funcs.append((v & 0xFFFF, iat + k * 4, None))
                else:
                    hint = struct.unpack_from("<H", self.data, self.rva_to_off(v))[0]
                    funcs.append((self.cstr(v + 2), iat + k * 4, hint))
                k += 1
                t += 4
            out.append((dll, funcs))
            off += 20
        return out

    def exports(self):
        if not self.dirs or not self.dirs[0][0]:
            return {}
        rva, size = self.dirs[0]
        off = self.rva_to_off(rva)
        (chars, ts, major, minor, name_rva, base, nfuncs, nnames,
         addr_funcs, addr_names, addr_ords) = struct.unpack_from("<IIHHIIIIIII", self.data, off)
        funcs = [struct.unpack_from("<I", self.data, self.rva_to_off(addr_funcs) + i * 4)[0]
                 for i in range(nfuncs)]
        names = {}
        for i in range(nnames):
            nrva = struct.unpack_from("<I", self.data, self.rva_to_off(addr_names) + i * 4)[0]
            ordn = struct.unpack_from("<H", self.data, self.rva_to_off(addr_ords) + i * 2)[0]
            names[self.cstr(nrva)] = funcs[ordn]
        return names

    # ------------------------------------------------------------ relocations
    def relocations(self):
        """[(rva, type, value_placeholder)] base relocations, type 3 = HIGHLOW."""
        out = []
        if len(self.dirs) < 6 or not self.dirs[5][0]:
            return out
        rva, size = self.dirs[5]
        end = rva + size
        while rva < end:
            page, block = struct.unpack_from("<II", self.read(rva, 8))
            if block < 8:
                break
            n = (block - 8) // 2
            for i in range(n):
                e = struct.unpack_from("<H", self.read(rva + 8 + i * 2, 2))[0]
                typ = e >> 12
                if typ:
                    out.append((page + (e & 0xFFF), typ))
            rva += block
        return out

    def has_relocations(self):
        return bool(self.dirs[5][0]) and not (self.characteristics & 0x0001)

    # -------------------------------------------------------------- resources
    def version_strings(self):
        """ProductName / FileVersion style strings from RT_VERSION, best effort."""
        if len(self.dirs) < 3 or not self.dirs[2][0]:
            return {}
        try:
            out = {}
            for data in self._walk_res(0, self.dirs[2][0], self.dirs[2][0] + self.dirs[2][1]):
                for key in (b"ProductName", b"FileVersion", b"ProductVersion",
                            b"OriginalFilename", b"FileDescription"):
                    i = data.find(key)
                    if i < 0:
                        continue
                    j = data.find(b"\0", i)
                    k = j + 1
                    # padded to 4 bytes, then a UTF-16 value
                    while k < len(data) and data[k] == 0:
                        k += 1
                    if k + 1 < len(data):
                        v = data[k:k + 512].decode("utf-16-le", "ignore").split("\0")[0]
                        out[key.decode()] = v
            return out
        except Exception:
            return {}

    def _walk_res(self, base, off, end):
        """Yield the raw bytes of every resource data entry (depth first)."""
        d = self.data
        n_named, n_id = struct.unpack_from("<HH", d, off + 12)
        for i in range(n_named + n_id):
            e = off + 16 + i * 8
            name, target = struct.unpack_from("<II", d, e)
            if target & 0x80000000:
                yield from self._walk_res(base, base + (target & 0x7FFFFFFF), end)
            else:
                drva, dsize, _cp, _r = struct.unpack_from("<IIII", d, base + target)
                doff = self.rva_to_off(drva)
                if doff is not None:
                    yield d[doff:doff + dsize]

    def debug_pdb(self):
        if len(self.dirs) < 7 or not self.dirs[6][0]:
            return None
        rva, size = self.dirs[6]
        off = self.rva_to_off(rva)
        for i in range(size // 28):
            (_c, _t, _maj, _min, typ, sz, addr, ptr) = struct.unpack_from(
                "<IIHHIIII", self.data, off + i * 28)
            if typ == 2 and sz >= 24:  # IMAGE_DEBUG_TYPE_CODEVIEW
                raw = self.data[ptr:ptr + sz]
                if raw[:4] == b"RSDS":
                    return raw[24:].split(b"\0")[0].decode("latin-1")
        return None

    def summary(self):
        return {
            "entry": self.entry,
            "image_base": self.image_base,
            "size": len(self.data),
            "size_of_image": self.size_of_image,
            "sections": [(s.name, s.va, s.vsize) for s in self.sections],
            "timestamp": self.timestamp,
            "pdb": self.debug_pdb(),
            "subsystem": self.subsystem,
            "relocs": len(self.relocations()),
            "overlay": len(self.overlay),
            "version": self.version_strings(),
        }
