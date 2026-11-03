"""D3DCompile (d3dcompiler_47.dll) and DXBC container checks, stdlib only.

The game generates HLSL at runtime and compiles it with the system
d3dcompiler_47.dll (D3DCompile, flags 0 for game shaders).  This module calls
the same entry point through ctypes so seed bytecode can be produced and
re-verified at build time, and implements the DXBC container checksum the
runtime loader also checks (rsx_shader_prep.h, rsp_dxbc_valid).
"""
import ctypes
import hashlib
import os
import struct
from ctypes import wintypes
from pathlib import Path

# ---------------------------------------------------------------------------
# DXBC container checksum: MD5 rounds with a non-standard final block.  The
# hashed bytes are the container after the 4-byte magic and 16-byte digest.
# ---------------------------------------------------------------------------
_S = [7, 12, 17, 22] * 4 + [5, 9, 14, 20] * 4 + [4, 11, 16, 23] * 4 + [6, 10, 15, 21] * 4
_K = [int(abs(__import__('math').sin(i + 1)) * 2 ** 32) & 0xFFFFFFFF for i in range(64)]


def _rotl(x, c):
    return ((x << c) | (x >> (32 - c))) & 0xFFFFFFFF


def _md5_block(state, block):
    m = struct.unpack('<16I', block)
    a, b, c, d = state
    for i in range(64):
        if i < 16:
            f, g = (b & c) | (~b & d), i
        elif i < 32:
            f, g = (d & b) | (~d & c), (5 * i + 1) % 16
        elif i < 48:
            f, g = b ^ c ^ d, (3 * i + 5) % 16
        else:
            f, g = c ^ (b | (~d & 0xFFFFFFFF)), (7 * i) % 16
        f = (f + a + _K[i] + m[g]) & 0xFFFFFFFF
        a, d, c = d, c, b
        b = (b + _rotl(f, _S[i])) & 0xFFFFFFFF
    return [(x + y) & 0xFFFFFFFF for x, y in zip(state, (a, b, c, d))]


def dxbc_checksum(data: bytes) -> bytes:
    """Digest that belongs at data[4:20] of a DXBC container."""
    body = data[20:]
    size = len(body)
    bits = (size * 8) & 0xFFFFFFFF
    full = size // 64
    state = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476]
    for i in range(full):
        state = _md5_block(state, body[i * 64:(i + 1) * 64])
    rest = body[full * 64:]
    tail = struct.pack('<I', (bits >> 2) | 1)
    if len(rest) >= 56:
        block = rest + b'\x80' + b'\0' * (63 - len(rest))
        state = _md5_block(state, block)
        block = struct.pack('<I', bits) + b'\0' * 56 + tail
    else:
        block = struct.pack('<I', bits) + rest + b'\x80'
        block += b'\0' * (60 - len(block)) + tail
    state = _md5_block(state, block)
    return struct.pack('<4I', *state)


def dxbc_valid(data: bytes) -> bool:
    """Container magic, declared size and checksum all consistent."""
    if len(data) < 32 or data[:4] != b'DXBC':
        return False
    declared = struct.unpack_from('<I', data, 24)[0]
    return declared == len(data) and dxbc_checksum(data) == data[4:20]


# ---------------------------------------------------------------------------
# d3dcompiler_47 through ctypes
# ---------------------------------------------------------------------------
class CompileError(RuntimeError):
    pass


_compiler = None


def compiler_path() -> Path:
    system = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'd3dcompiler_47.dll'
    return system


def _load():
    global _compiler
    if _compiler is None:
        dll = ctypes.WinDLL(str(compiler_path()))
        fn = dll.D3DCompile
        fn.restype = ctypes.c_long
        fn.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p,
                       ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, wintypes.UINT,
                       wintypes.UINT, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]
        _compiler = fn
    return _compiler


def _blob_bytes(blob):
    vtbl = ctypes.cast(ctypes.cast(blob, ctypes.POINTER(ctypes.c_void_p))[0],
                       ctypes.POINTER(ctypes.c_void_p))
    get_ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vtbl[3])
    get_size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vtbl[4])
    release = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vtbl[2])
    data = ctypes.string_at(get_ptr(blob), get_size(blob))
    release(blob)
    return data


def compile_hlsl(source: bytes, entry: str = 'main', target: str = 'ps_5_0', flags: int = 0,
                 name: str | None = None) -> bytes:
    """Exactly what the runtime does: D3DCompile(src, len, name, NULL, NULL, entry, target, flags, 0)."""
    fn = _load()
    code = ctypes.c_void_p()
    errors = ctypes.c_void_p()
    buf = ctypes.create_string_buffer(source, len(source))
    hr = fn(buf, len(source), name.encode() if name else None, None, None, entry.encode(),
            target.encode(), flags, 0, ctypes.byref(code), ctypes.byref(errors))
    message = _blob_bytes(errors).decode('utf-8', 'replace') if errors.value else ''
    if hr < 0 or not code.value:
        raise CompileError(f'D3DCompile hr=0x{hr & 0xFFFFFFFF:08X}: {message[:2000]}')
    return _blob_bytes(code)


def compiler_identity() -> dict:
    """File version and content digest of the d3dcompiler used for seed builds."""
    path = compiler_path()
    data = path.read_bytes()
    size = ctypes.windll.version.GetFileVersionInfoSizeW(str(path), None)
    version = None
    if size:
        info = ctypes.create_string_buffer(size)
        if ctypes.windll.version.GetFileVersionInfoW(str(path), 0, size, info):
            pointer = ctypes.c_void_p()
            length = wintypes.UINT()
            if ctypes.windll.version.VerQueryValueW(info, '\\', ctypes.byref(pointer), ctypes.byref(length)):
                fixed = ctypes.string_at(pointer, length.value)
                ms, ls = struct.unpack_from('<II', fixed, 8)
                version = f'{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}.{ls & 0xFFFF}'
    return {'file': path.name, 'version': version, 'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)}
