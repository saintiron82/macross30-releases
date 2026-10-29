"""Generate game-derived inputs only from a user-supplied, hash-checked ELF.

No decryption keys, precompiled game objects or generated PPU/SPU sources are
accepted from the network. This does not assert permission to use a game.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
ORIGINAL_SHA256 = '44d7112cbb5527314eb6192e59d861dacfcd4099f301a6e82ffc1681b0782b5b'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(original, payload=None):
    original = Path(original).resolve()
    if original.read_bytes()[:4] != b'\x7fELF' or sha(original) != ORIGINAL_SHA256:
        raise ValueError('Original ELF identity mismatch. Encrypted SELF is not a decrypted ELF; no bundled replacement is used.')
    destination = ROOT / 'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf'
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.resolve() != original:
        shutil.copyfile(original, destination)
    raw = original.read_bytes()
    recipe = json.loads((ROOT / 'tools/local_build/raw_jobs.json').read_text(encoding='utf8'))
    target = ROOT / 'native/macross30/raw_jobs'
    target.mkdir(parents=True, exist_ok=True)
    for row in recipe:
        data = raw[row['offset']:row['offset'] + row['size']]
        if len(data) != row['size'] or hashlib.sha256(data).hexdigest() != row['sha256']:
            raise ValueError('Original SPU slice mismatch: ' + row['file'])
        (target / row['file']).write_bytes(data)
    guards = json.loads((ROOT / 'tools/local_build/code_guards.json').read_text(encoding='utf8'))
    header = '/* Generated exactoriginalcode; no modifiedguestbytes. */\n'
    for row in guards['arrays']:
        code = raw[row['offset']:row['offset'] + row['size']]
        if hashlib.sha256(code).hexdigest() != row['sha256']:
            raise ValueError('User original code guard mismatch: ' + row['name'])
        header += 'static const unsigned char ' + row['name'] + '[' + str(len(code)) + ']={\n'
        for start in range(0, len(code), 32):
            header += ','.join('0x%02x' % value for value in code[start:start+32]) + ',\n'
        header += '};\n'
    if hashlib.sha256(header.encode()).hexdigest() != guards['sha256']:
        raise ValueError('Generated original code guard header mismatch')
    (ROOT / 'native/macross30' / guards['file']).write_text(header, encoding='utf8', newline='\n')
    # This first source-only build has no bundled voice/dialogue table.
    # A synthetic empty key keeps the independent lookup implementation valid.
    catalog = '{"local_empty/000000", ""},\n'
    if payload is not None:
        rows = json.loads((Path(payload) / 'battle-captions.json').read_text(encoding='utf8'))
        if not rows or len(rows) > 50000:
            raise ValueError('Invalid caption data')
        catalog = ''.join('{' + json.dumps(key, ensure_ascii=False) + ', ' + json.dumps(text, ensure_ascii=False) + '},\n' for key, text in rows)
    (ROOT / 'native/macross30/battle_voice_catalog.inc').write_text(catalog, encoding='utf8')
    (ROOT / 'native/analysis').mkdir(parents=True, exist_ok=True)
    print('Validated user ELF; raw jobs generated locally; no downloaded game-code fallback', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original-elf', required=True, type=Path)
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--payload', type=Path)
    args = parser.parse_args()
    prepare(args.original_elf, args.payload)
    if args.prepare_only:
        return
    commands = [
        ['vendor/ps3recomp-master/tools/ppu_loader.py', 'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf', '-o', 'native/analysis'],
        ['vendor/ps3recomp-master/tools/ppu_lifter.py', 'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf', '--functions', 'native/analysis/EBOOT.functions.json', '--hle-stubs', 'native/analysis/EBOOT.imports.json', '--code-end', '0x8ddf4c', '--chunks', '96', '--jobs', '4', '--output', 'native/macross30/recompiled'],
        ['tools/build/lift_native_job.py'], ['tools/build/lift_graphics_job.py'], ['tools/build/lift_gameplay_jobs.py', '--merge-chunks'],
        *(['tools/build/lift_gameplay_jobs.py', '--only', label] for label in ('jobcrt14', 'jobcrt14_b', 'jobcrt14_d')),
        ['tools/build/patch_geom2_local_dispatch.py', '--apply'],
        ['tools/local_build/apply_hooks.py'], ['tools/local_build/posttask.py'],
    ]
    for command in commands:
        print('LOCAL GENERATE: ' + command[0], flush=True)
        subprocess.run([sys.executable, '-X', 'utf8', *command], cwd=ROOT, check=True)
    print('All PPU/SPU sources generated locally', flush=True)


if __name__ == '__main__':
    main()
