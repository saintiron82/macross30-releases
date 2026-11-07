"""Generate exhaustive static entries for reviewed Macross SPU code spans.

These structural checks do not prove semantic equivalence or runtime coverage.
Guest bytes, executable bounds, and fingerprints remain unchanged.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'vendor/ps3recomp-master/tools'))
from find_spu_functions import parse_elf, text_image
from spu_disasm import disassemble_spu

BOUNDS = {
    'b00089288811e0386af134b05f015fe4dd39173f00250001bf158290adbf67b8': 0xb2a0,
    '2f512e1a72325554e198ffd3ae1e76a3b349caa24715ffdd8cf4b0873b32a3ff': 0x5fc8,
    '9d9d89391357928b257cb9f0fbee7733ca22968c798f292db13b36f308c82158': 0xad30,
    'c068f107cb21d183649f2d435ba197e9997753bfea23f94d0958716a99b6946f': 0x11f30,
}


def prepare_entries(image, output):
    data = Path(image).read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    end = BOUNDS[digest]
    code, base = text_image(data, parse_elf(data)['phs'])
    assert end > base and (end - base) % 4 == 0
    assert code[end - base - 4:end - base] == bytes.fromhex('35000000')
    instructions = disassemble_spu(code[:end - base], base)
    unsupported = [dict(address=hex(i.addr), word=hex(i.raw))
                   for i in instructions if i.mnemonic == '.word']
    if unsupported:
        raise ValueError(f'Undecodable words in reviewed span: {unsupported}')
    addresses = list(range(base, end, 4))
    assert [i.addr for i in instructions] == addresses
    functions = [dict(start=hex(a), end=hex(a + 4)) for a in addresses]
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    manifest = output / 'static_entries.functions.json'
    manifest.write_text(json.dumps(functions, indent=2), encoding='utf-8')
    report = dict(source=str(Path(image).resolve()), sha256=digest,
                  code_base=hex(base), code_end=hex(end),
                  entry_count=len(functions), entry_stride=4,
                  unsupported_decode=unsupported, guest_bytes_modified=False,
                  semantic_equivalence_verified=False, runtime_coverage_verified=False,
                  warning='Aligned entries cover reviewed code span only. Decode success '
                          'does not prove code/data classification or instruction semantics.')
    (output / 'static_entries.audit.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    return manifest, report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path)
    parser.add_argument('--output', type=Path, default=ROOT / 'native/analysis/static_spu_entries')
    args = parser.parse_args()
    images = [args.image] if args.image else sorted((ROOT / 'analysis/spu').glob('*.elf'))
    for image in images:
        out = args.output if args.image else args.output / image.stem
        _, report = prepare_entries(image, out)
        print(json.dumps(report, ensure_ascii=False))
