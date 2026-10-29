"""Recreate guarded posttask kernels from the user's original ELF, without captures."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    elf = (ROOT / 'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf').read_bytes()
    recipes = json.loads((ROOT / 'tools/local_build/posttask.json').read_text(encoding='utf8'))
    guard = (ROOT / 'tools/local_build/posttask_guard.txt').read_text(encoding='utf8')
    target = ROOT / 'native/macross30/spu_posttask_native'
    target.mkdir(exist_ok=True)
    for row in recipes:
        family = row['family']
        code = elf[row['offset']:row['offset'] + row['size']]
        if hashlib.sha256(code).hexdigest() != row['sha256']:
            raise ValueError('Original posttask code mismatch')
        work = ROOT / 'native/analysis/local-posttask' / str(family)
        work.mkdir(parents=True, exist_ok=True)
        (work / 'ls.bin').write_bytes(bytes(0x6230) + code)
        (work / 'bounds.json').write_text(json.dumps(row['bounds']), encoding='utf8')
        subprocess.run([sys.executable, '-X', 'utf8', str(ROOT / 'vendor/ps3recomp-master/tools/spu_lifter.py'),
                        str(work / 'ls.bin'), '--functions', str(work / 'bounds.json'), '--output', str(work),
                        '--symbol-prefix', 'wide_'], check=True)
        header = (work / 'spu_recomp.h').read_text(encoding='utf8').replace('../../runtime/spu/', '').replace('wide_', f'macross_posttask_f{family}_').replace('"spu_context.h"', '"../../../vendor/ps3recomp-master/runtime/spu/spu_context.h"')
        source = (work / 'spu_recomp.c').read_text(encoding='utf8').replace('../../runtime/spu/', '').split('/* Function table */')[0].replace('wide_', f'macross_posttask_f{family}_').replace('"spu_recomp.h"', f'"family{family}.h"').replace('"spu_helpers.h"', '"../../../vendor/ps3recomp-master/runtime/spu/spu_helpers.h"')
        if hashlib.sha256(source.encode()).hexdigest() != row['input_sha256']:
            raise ValueError('Posttask lifter output changed')
        lines = source.splitlines(keepends=True)
        for edit in reversed(row['edits']):
            start, count = edit['start'], edit['remove']
            if hashlib.sha256(''.join(lines[start:start+count]).encode()).hexdigest() != edit['removed_sha256']:
                raise ValueError('Posttask host hook guard failed')
            lines[start:start+count] = [edit['insert']]
        source = ''.join(lines)
        if hashlib.sha256(source.encode()).hexdigest() != row['output_sha256']:
            raise ValueError('Posttask final source mismatch')
        (target / f'family{family}.c').write_text(source, encoding='utf8', newline='\n')
        (target / f'family{family}.h').write_text(header, encoding='utf8', newline='\n')
        guard = guard.replace(f'LOCAL_EXPECTED_{family}\n', f'static const unsigned char expected{family}[]={{' + ','.join(map(str, code)) + '};\n')
    if 'LOCAL_EXPECTED_' in guard:
        raise ValueError('Incomplete local guard generation')
    (target / 'guard.c').write_text(guard, encoding='utf8', newline='\n')
    shutil.copyfile(ROOT / 'tools/local_build/boundary_perf.h', target / 'boundary_perf.h')
    print('LOCAL_POSTTASK_COMPLETE: nine original guarded kernels generated locally')


if __name__ == '__main__':
    main()
