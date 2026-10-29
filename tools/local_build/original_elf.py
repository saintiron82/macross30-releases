"""Prepare original ELF locally from the user's ISO through their RPCS3 tool.

No game executes and no firmware/keys/ELF files are downloaded by this script.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/play'))
from inspect_iso import inventory, read_file
from prepare import ORIGINAL_SHA256


def extract(iso, destination, rpcs3):
    destination = Path(destination).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    with Path(iso).open('rb') as stream:
        entry = next((row for row in inventory(stream) if row['path'].casefold() == 'ps3_game/usrdir/eboot.bin'), None)
        if entry is None or entry['size'] > 32 * 1024 * 1024:
            raise ValueError('Supported user ISO EBOOT not found')
        data = read_file(stream, entry)
    plain = destination / 'EBOOT.elf'
    if data[:4] == b'\x7fELF':
        plain.write_bytes(data)
    else:
        if data[:4] != b'SCE\0' or not Path(rpcs3).is_file():
            raise ValueError('User-local SELF tool is required; no downloaded game ELF fallback')
        encrypted = destination / 'EBOOT.BIN'
        encrypted.write_bytes(data)
        result = subprocess.run([str(Path(rpcs3).resolve()), '--decrypt', str(encrypted)], cwd=destination,
                                input=b'', stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90,
                                creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0)
        (destination / 'original-tool.log').write_bytes(result.stdout)
        if result.returncode or not plain.exists():
            raise ValueError('User-local original preparation failed')
    if hashlib.sha256(plain.read_bytes()).hexdigest() != ORIGINAL_SHA256:
        raise ValueError('Prepared original ELF does not match supported user game')
    return plain


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--rpcs3', type=Path)
    args = parser.parse_args()
    path = extract(args.iso, args.output, args.rpcs3 or Path('missing-user-tool'))
    print('LOCAL_ORIGINAL_READY: ' + str(path), flush=True)


if __name__ == '__main__':
    main()
