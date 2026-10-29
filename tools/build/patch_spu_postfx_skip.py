"""Make the lifted Edge MLAA SPU task skip its compute kernels while the GPU
post-process is active (native/macross30/spu_postfx_skip.c decides).

With the internal render scale the frame is post-processed on the GPU and the
scene is never written back to guest memory, so the SPU MLAA works on a stale
copy nobody shows (screenshots identical with and without it, 2026-10-25). The
three kernels (entries 0x73F0, 0x8230, 0x8680) are leaf functions without
channel or DMA operations; returning at entry keeps the task's own DMA,
signalling and completion untouched.

Idempotent; run after the SPU sources are generated (CMakeLists.txt does).
Usage: patch_spu_postfx_skip.py <generated spu dir>
"""
import sys
from pathlib import Path

IMAGE = 'spu_b00089288811e0386af134b05f015fe4dd39173f00250001bf158290adbf67b8'
KERNELS = ('000073F0', '00008230', '00008680')
MARK = '/* spu_postfx_skip */'


def main():
    src = Path(sys.argv[1]) / IMAGE / 'spu_recomp.c'
    if not src.exists():
        print(f'patch_spu_postfx_skip: {src} not found, nothing to do')
        return
    text = src.read_text(encoding='utf-8')
    if MARK in text:
        return
    prefix = f'void {IMAGE}_spu_func_'
    first = text.index(prefix)
    text = text[:first] + f'{MARK}\nextern int macross_spu_postfx_skip(void);\n' + text[first:]
    for kernel in KERNELS:
        head = f'{prefix}{kernel}(spu_context* ctx) {{\n'
        if text.count(head) != 1:
            sys.exit(f'patch_spu_postfx_skip: kernel {kernel} not found once in {src}')
        text = text.replace(head, head + '        if (macross_spu_postfx_skip()) { SPU_RET(ctx); }\n')
    src.write_text(text, encoding='utf-8')
    print(f'patch_spu_postfx_skip: patched {len(KERNELS)} MLAA kernels in {src}')


if __name__ == '__main__':
    main()
