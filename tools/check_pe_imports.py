#!/usr/bin/env python3
"""Check a Windows PE (.dll/.exe/.vst3) for dependencies an end user may not have.

A VST3 that imports MSVCP140.dll / VCRUNTIME140*.dll needs the Visual C++ 2015-2022
Redistributable; on a machine without it the plugin silently fails to load and the DAW
only says "failed to load" or "import failed". Building with a static MSVC runtime
(CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded) removes those imports.

Pure stdlib, runs on any OS, so CI can check Windows binaries from any runner.

  python3 tools/check_pe_imports.py <file> [--list]
exit 0 = no forbidden imports, 1 = forbidden import found, 2 = not a PE / parse error
"""
import struct, sys

FORBIDDEN_PREFIXES = ("MSVCP", "VCRUNTIME", "MSVCR", "CONCRT")

def imports(path):
    f = open(path, "rb").read()
    if f[:2] != b"MZ":
        raise ValueError("not a PE file (no MZ header)")
    pe = struct.unpack_from("<I", f, 0x3C)[0]
    if f[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file (no PE signature)")
    machine, nsec, _, _, _, optsz, _ = struct.unpack_from("<HHIIIHH", f, pe + 4)
    opt = pe + 24
    magic = struct.unpack_from("<H", f, opt)[0]
    ddir = opt + (112 if magic == 0x20B else 96)
    imp_rva = struct.unpack_from("<I", f, ddir + 8)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + optsz + i * 40
        vsz, va, rsz, ptr = struct.unpack_from("<IIII", f, o + 8)
        secs.append((va, vsz, ptr, rsz))
    def r2o(rva):
        for va, vsz, ptr, rsz in secs:
            if va <= rva < va + max(vsz, rsz):
                return ptr + (rva - va)
        raise ValueError(f"RVA 0x{rva:x} is outside every section")
    out, off = [], r2o(imp_rva)
    while True:
        _, _, _, name_rva, _ = struct.unpack_from("<IIIII", f, off)
        if name_rva == 0:
            break
        o = r2o(name_rva)
        out.append(f[o:f.index(b"\0", o)].decode("ascii", "replace"))
        off += 20
    arch = {0x8664: "x64", 0x14C: "x86", 0xAA64: "arm64"}.get(machine, hex(machine))
    return arch, out

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        print(__doc__); return 2
    path = args[0]
    try:
        arch, dlls = imports(path)
    except Exception as e:
        print(f"ERROR {path}: {e}"); return 2
    bad = sorted({d for d in dlls if d.upper().startswith(FORBIDDEN_PREFIXES)})
    print(f"{path}: {arch}, {len(dlls)} imported DLLs")
    if "--list" in sys.argv:
        for d in sorted(dlls): print("   ", d)
    if bad:
        print("FAIL: dynamic MSVC runtime imports (needs the VC++ redistributable on the user's machine):")
        for d in bad: print("   ", d)
        print("Fix: build with CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded (static CRT).")
        return 1
    print("ok: no dynamic MSVC runtime dependency")
    return 0

if __name__ == "__main__":
    sys.exit(main())
