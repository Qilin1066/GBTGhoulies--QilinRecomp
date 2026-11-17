#!/usr/bin/env python3
"""Minimal XBE (original Xbox executable) inspector.

Usage: xbeinfo.py default.xbe [--json out.json]
Prints image header, certificate, sections, library versions, kernel imports.
"""
import json
import struct
import sys

RETAIL_EP_KEY = 0xA8FC57AB
DEBUG_EP_KEY = 0x94859D4B
RETAIL_KT_KEY = 0x5B6D40B6
DEBUG_KT_KEY = 0xEFB1F152


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


class Xbe:
    def __init__(self, data):
        self.d = data
        if data[:4] != b"XBEH":
            raise ValueError("not an XBE")
        self.base = u32(data, 0x104)
        self.size_headers = u32(data, 0x108)
        self.size_image = u32(data, 0x10C)
        self.timestamp = u32(data, 0x114)
        self.cert_addr = u32(data, 0x118)
        self.num_sections = u32(data, 0x11C)
        self.sect_addr = u32(data, 0x120)
        self.init_flags = u32(data, 0x124)
        ep_raw = u32(data, 0x128)
        self.tls_addr = u32(data, 0x12C)
        self.stack_commit = u32(data, 0x130)
        self.heap_reserve = u32(data, 0x134)
        self.heap_commit = u32(data, 0x138)
        self.debug_path_addr = u32(data, 0x14C)
        self.debug_file_addr = u32(data, 0x150)
        kt_raw = u32(data, 0x158)
        self.num_libs = u32(data, 0x160)
        self.libs_addr = u32(data, 0x164)

        self.sections = []
        for i in range(self.num_sections):
            o = self.hdr_off(self.sect_addr) + i * 56
            flags, va, vsz, raw, rsz, name_addr = struct.unpack_from("<6I", data, o)
            self.sections.append(dict(
                name=self.cstr_hdr(name_addr), flags=flags, va=va, vsize=vsz,
                raw=raw, rsize=rsz,
                writable=bool(flags & 1), preload=bool(flags & 2),
                executable=bool(flags & 4), inserted=bool(flags & 8),
            ))

        # Decide retail vs debug by which key gives an entry point inside an executable section
        self.kind = None
        for kind, epk, ktk in (("retail", RETAIL_EP_KEY, RETAIL_KT_KEY),
                               ("debug", DEBUG_EP_KEY, DEBUG_KT_KEY)):
            ep = ep_raw ^ epk
            if any(s["executable"] and s["va"] <= ep < s["va"] + s["vsize"] for s in self.sections):
                self.kind, self.entry, self.kthunk = kind, ep, kt_raw ^ ktk
                break
        if self.kind is None:
            raise ValueError("could not decode entry point with retail or debug key")

    # header-area addresses are base-relative
    def hdr_off(self, va):
        return va - self.base

    def cstr_hdr(self, va):
        o = self.hdr_off(va)
        e = self.d.index(b"\0", o)
        return self.d[o:e].decode("latin-1")

    def va_to_off(self, va):
        if self.base <= va < self.base + self.size_headers:
            return va - self.base
        for s in self.sections:
            if s["va"] <= va < s["va"] + s["rsize"]:
                return s["raw"] + (va - s["va"])
        return None

    def certificate(self):
        o = self.hdr_off(self.cert_addr)
        title_id = u32(self.d, o + 0x08)
        name = self.d[o + 0x0C:o + 0x0C + 80].decode("utf-16-le").split("\0")[0]
        return dict(
            title_id=f"{title_id:08X}",
            title_id_str=f"{chr(title_id >> 24)}{chr((title_id >> 16) & 0xFF)}-{title_id & 0xFFFF:03d}",
            title_name=name,
            allowed_media=f"{u32(self.d, o + 0x9C):08X}",
            region=f"{u32(self.d, o + 0xA0):08X}",
            ratings=u32(self.d, o + 0xA4),
            disk_number=u32(self.d, o + 0xA8),
            version=u32(self.d, o + 0xAC),
        )

    def libraries(self):
        out = []
        o = self.hdr_off(self.libs_addr)
        for i in range(self.num_libs):
            p = o + i * 16
            name = self.d[p:p + 8].split(b"\0")[0].decode("latin-1")
            maj, mnr, bld, fl = struct.unpack_from("<4H", self.d, p + 8)
            out.append(dict(name=name, version=f"{maj}.{mnr}.{bld}",
                            qfe=fl & 0x1FFF, approved=(fl >> 13) & 3, debug=bool(fl >> 15)))
        return out

    def kernel_imports(self):
        o = self.va_to_off(self.kthunk)
        ords = []
        while True:
            v = u32(self.d, o)
            if v == 0:
                break
            ords.append(v & 0x7FFFFFFF if v & 0x80000000 else v)
            o += 4
        return ords


def main():
    path = sys.argv[1]
    x = Xbe(open(path, "rb").read())
    info = dict(
        kind=x.kind,
        base=f"{x.base:08X}",
        entry=f"{x.entry:08X}",
        kernel_thunk=f"{x.kthunk:08X}",
        size_image=x.size_image,
        stack_commit=x.stack_commit,
        heap_reserve=x.heap_reserve,
        heap_commit=x.heap_commit,
        init_flags=f"{x.init_flags:08X}",
        tls_addr=f"{x.tls_addr:08X}",
        timestamp=x.timestamp,
        debug_path=x.cstr_hdr(x.debug_path_addr) if x.debug_path_addr else None,
        certificate=x.certificate(),
        sections=[{k: (f"{v:08X}" if k in ("va", "raw", "flags") else v) for k, v in s.items()} for s in x.sections],
        libraries=x.libraries(),
        kernel_imports=x.kernel_imports(),
    )
    if "--json" in sys.argv:
        json.dump(info, open(sys.argv[sys.argv.index("--json") + 1], "w"), indent=2)
    print(json.dumps(info, indent=2))


if __name__ == "__main__":
    main()
