"""Thin wrapper over LLVM's C disassembler API (libLLVM 18), i386, Intel syntax.

Only text is available from the C API, so this module also does light operand
parsing for control flow. Used by analysis tools; the production lifter will use
a structured decoder.
"""
import ctypes as C
import glob
import re

_LIB = None


def _lib():
    global _LIB
    if _LIB is None:
        cands = sorted(glob.glob("/usr/lib/llvm-*/lib/libLLVM.so.*"))
        L = C.CDLL(cands[-1])
        for f in ("LLVMInitializeX86TargetInfo", "LLVMInitializeX86TargetMC", "LLVMInitializeX86Disassembler"):
            getattr(L, f)()
        L.LLVMCreateDisasm.restype = C.c_void_p
        L.LLVMCreateDisasm.argtypes = [C.c_char_p, C.c_void_p, C.c_int, C.c_void_p, C.c_void_p]
        L.LLVMSetDisasmOptions.argtypes = [C.c_void_p, C.c_uint64]
        L.LLVMDisasmInstruction.restype = C.c_size_t
        L.LLVMDisasmInstruction.argtypes = [C.c_void_p, C.c_char_p, C.c_uint64, C.c_uint64, C.c_char_p, C.c_size_t]
        _LIB = L
    return _LIB


OPT_IMM_HEX = 2
OPT_INTEL = 4

BRANCH_JCC = {"jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns", "jp", "jnp",
              "jl", "jge", "jle", "jg", "jcxz", "jecxz", "loop", "loope", "loopne"}


class Insn:
    __slots__ = ("addr", "size", "mnem", "ops", "text")

    def __init__(self, addr, size, text):
        self.addr, self.size, self.text = addr, size, text
        parts = text.split(None, 1)
        m = parts[0] if parts else ""
        ops = parts[1] if len(parts) > 1 else ""
        while m in ("rep", "repne", "repe", "lock", "data16") and ops:
            p = ops.split(None, 1)
            m = m + " " + p[0]
            ops = p[1] if len(p) > 1 else ""
        self.mnem, self.ops = m, ops.strip()

    @property
    def end(self):
        return self.addr + self.size

    def rel_target(self):
        """Target of a direct jmp/jcc/call (LLVM prints PC-relative displacement)."""
        if re.fullmatch(r"-?0x[0-9a-f]+|-?\d+", self.ops):
            return (self.end + int(self.ops, 0)) & 0xFFFFFFFF
        return None

    def __repr__(self):
        return f"{self.addr:08x}: {self.text}"


class Decoder:
    def __init__(self, image, base):
        self.img, self.base = image, base
        L = _lib()
        self.L = L
        self.dc = L.LLVMCreateDisasm(b"i386-pc-windows", None, 0, None, None)
        L.LLVMSetDisasmOptions(self.dc, OPT_INTEL | OPT_IMM_HEX)
        self.buf = C.create_string_buffer(256)
        self.cache = {}

    def decode(self, addr):
        ins = self.cache.get(addr)
        if ins is not None:
            return ins
        o = addr - self.base
        if o < 0 or o >= len(self.img):
            return None
        chunk = self.img[o:o + 15]
        n = self.L.LLVMDisasmInstruction(self.dc, chunk, len(chunk), addr, self.buf, 256)
        if n == 0:
            self.cache[addr] = None
            return None
        ins = Insn(addr, n, self.buf.value.decode("latin-1").strip())
        self.cache[addr] = ins
        return ins
