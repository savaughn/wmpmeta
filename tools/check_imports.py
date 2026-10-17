#!/usr/bin/env python3
"""Verify a PE32 executable only imports functions that exist on Windows 2000.

    check_imports.py <exe> <win2k-system32-dir | exports-list.txt>
    check_imports.py --list <win2k-system32-dir> <dll>... > exports-list.txt

The system32 directory must hold real Windows 2000 DLLs (kernel32.dll,
msvcrt.dll, ...). Where those can't be shipped (CI), a text file of
"dll name" lines made with --list from the real DLLs stands in for them. Every DLL the exe imports must be present there, and every
imported name must be in that DLL's export table (forwarders count). The
exe's OS and subsystem version fields must also be 5.0 or lower, because the
Win2k loader refuses anything newer.
"""
import os
import struct
import sys


class PE:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.d = f.read()
        d = self.d
        if d[:2] != b"MZ":
            raise ValueError(f"{path}: not an MZ file")
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError(f"{path}: no PE signature")
        self.machine, nsec = struct.unpack_from("<HH", d, pe + 4)
        opt_size = struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", d, opt)[0]
        if magic != 0x10B:
            raise ValueError(f"{path}: not PE32")
        (self.os_major, self.os_minor) = struct.unpack_from("<HH", d, opt + 40)
        (self.sub_major, self.sub_minor) = struct.unpack_from("<HH", d, opt + 48)
        self.subsystem = struct.unpack_from("<H", d, opt + 68)[0]
        self.dirs = [struct.unpack_from("<II", d, opt + 96 + 8 * i) for i in range(16)]
        self.sections = []
        s = opt + opt_size
        for i in range(nsec):
            vsize, va, rsize, raw = struct.unpack_from("<IIII", d, s + 40 * i + 8)
            self.sections.append((va, max(vsize, rsize), raw))

    def off(self, rva):
        for va, size, raw in self.sections:
            if va <= rva < va + size:
                return rva - va + raw
        raise ValueError(f"rva {rva:#x} outside sections")

    def cstr(self, rva):
        o = self.off(rva)
        return self.d[o:self.d.index(b"\0", o)].decode("latin-1")

    def exports(self):
        rva, size = self.dirs[0]
        if not rva:
            return set(), set()
        o = self.off(rva)
        base, nfunc, nnames, _, names_rva, _ = struct.unpack_from("<IIIIII", self.d, o + 16)
        names = set()
        for i in range(nnames):
            names.add(self.cstr(struct.unpack_from("<I", self.d, self.off(names_rva) + 4 * i)[0]))
        ords = set(range(base, base + nfunc))
        return names, ords

    def imports(self):
        rva, _ = self.dirs[1]
        out = {}
        o = self.off(rva)
        while True:
            ilt, _, _, name_rva, iat = struct.unpack_from("<IIIII", self.d, o)
            if not name_rva:
                break
            dll = self.cstr(name_rva)
            thunk = self.off(ilt or iat)
            syms = out.setdefault(dll.lower(), [])
            while True:
                t = struct.unpack_from("<I", self.d, thunk)[0]
                if not t:
                    break
                syms.append(("#%d" % (t & 0xFFFF)) if t & 0x80000000 else self.cstr(t + 2))
                thunk += 4
            o += 20
        return out


class ExportList:
    """Exports read from a --list file instead of the DLLs themselves."""

    def __init__(self, path):
        self.by_dll = {}
        with open(path) as f:
            for line in f:
                if line.strip() and not line.startswith("#"):
                    dll, name = line.split()
                    self.by_dll.setdefault(dll.lower(), set()).add(name)

    def exports(self, dll):
        return self.by_dll.get(dll), set()


def list_exports(sysdir, dlls):
    print("# Exports of the Windows 2000 SP4 DLLs wmpmeta imports from, made with")
    print("# tools/check_imports.py --list. Names only; used by CI's import check.")
    for dll in dlls:
        names, _ = PE(os.path.join(sysdir, dll)).exports()
        for n in sorted(names):
            print(f"{dll.lower()} {n}")


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == "--list":
        list_exports(sys.argv[2], sys.argv[3:])
        return
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    exe, sysdir = sys.argv[1], sys.argv[2]
    listed = ExportList(sysdir) if os.path.isfile(sysdir) else None
    pe = PE(exe)
    errors = []
    if pe.machine != 0x14C:
        errors.append(f"machine {pe.machine:#x} is not i386")
    for label, ver in (("OS", (pe.os_major, pe.os_minor)), ("subsystem", (pe.sub_major, pe.sub_minor))):
        if ver > (5, 0):
            errors.append(f"{label} version {ver[0]}.{ver[1]} > 5.0; the Win2k loader rejects it")

    total = 0
    for dll, syms in sorted(pe.imports().items()):
        if listed:
            names, ords = listed.exports(dll)
            if names is None:
                errors.append(f"{dll}: not in {sysdir}")
                continue
        else:
            path = os.path.join(sysdir, dll)
            if not os.path.exists(path):
                errors.append(f"{dll}: not present in {sysdir}")
                continue
            names, ords = PE(path).exports()
        for s in syms:
            total += 1
            ok = int(s[1:]) in ords if s.startswith("#") else s in names
            if not ok:
                errors.append(f"{dll}!{s}: not exported by the Windows 2000 DLL")
        print(f"  {dll:14s} {len(syms):3d} imports")

    if errors:
        print("FAIL:")
        for e in errors:
            print("  " + e)
        sys.exit(1)
    print(f"OK: {total} imports, all exported by Windows 2000 ({sysdir}); "
          f"OS {pe.os_major}.{pe.os_minor}, subsystem {pe.sub_major}.{pe.sub_minor}")


if __name__ == "__main__":
    main()
