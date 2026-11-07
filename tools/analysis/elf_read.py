"""Read big-endian words from EBOOT.elf by guest EA. Usage: elf_read.py EA [count]"""
import struct, sys
from pathlib import Path
d = (Path(__file__).resolve().parents[2] / 'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf').read_bytes()
phoff, = struct.unpack_from('>Q', d, 0x20); phnum, = struct.unpack_from('>H', d, 0x38)
segs = []
for i in range(phnum):
    t, f, off, va, pa, fs, ms, al = struct.unpack_from('>IIQQQQQQ', d, phoff + i * 56)
    if t == 1: segs.append((va, off, fs))
def word(ea):
    for va, off, fs in segs:
        if va <= ea < va + fs: return struct.unpack_from('>I', d, off + ea - va)[0]
    return None
ea = int(sys.argv[1], 16); n = int(sys.argv[2]) if len(sys.argv) > 2 else 8
for k in range(n):
    w = word(ea + 4 * k); print(f'{ea + 4 * k:08X}: {w:08X}' if w is not None else f'{ea+4*k:08X}: ----')
