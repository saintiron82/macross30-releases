"""Prepare original ELF locally from the user's ISO through their RPCS3 tool.

No game executes and no firmware/keys/ELF files are downloaded by this script.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/play'))
from inspect_iso import inventory, read_file
ORIGINAL_SHA256 = '44d7112cbb5527314eb6192e59d861dacfcd4099f301a6e82ffc1681b0782b5b'


def extract(iso, destination, rpcs3):
    destination = Path(destination).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    with Path(iso).open('rb') as stream:
        entry = next((row for row in inventory(stream) if row['path'].casefold() == 'ps3_game/usrdir/eboot.bin'), None)
        if entry is None or entry['size'] > 32 * 1024 * 1024:
            raise ValueError('Supported user ISO EBOOT not found')
        data = read_file(stream, entry)
    plain = destination / 'EBOOT.elf'
    tool_present = rpcs3 is not None and Path(rpcs3).is_file()
    print('ORIGINAL_EBOOT_INPUT: magic=' + data[:4].hex() +
          ' bytes=' + str(len(data)) + ' local_tool_present=' + str(tool_present).lower(), flush=True)
    if data[:4] == b'\x7fELF':
        prepared = data
    else:
        if data[:4] != b'SCE\0':
            raise ValueError('Unsupported ISO EBOOT header ' + data[:4].hex() +
                             '; expected ELF or SCE SELF. Check that the selected game ISO is decrypted and readable.')
        if not tool_present:
            raise ValueError('Original preparation tool missing: .local-build/tools/rpcs3/rpcs3.exe. '
                             'Run installation again to prepare the official utility.')
        # Validate a fresh result before replacing an existing cache. A stale
        # EBOOT.elf must never make a successful tool exit look like decryption.
        with tempfile.TemporaryDirectory(prefix='original-', dir=destination) as work:
            encrypted = Path(work) / 'EBOOT.BIN'
            encrypted.write_bytes(data)
            result = subprocess.run([str(Path(rpcs3).resolve()), '--decrypt', str(encrypted)], cwd=work,
                                    input=b'', stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90,
                                    creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0)
            (destination / 'original-tool.log').write_bytes(result.stdout)
            result_elf = Path(work) / 'EBOOT.elf'
            if result.returncode or not result_elf.is_file():
                raise ValueError('Original SELF preparation failed (exit=' + str(result.returncode) +
                                 '); see .local-build/original/original-tool.log')
            prepared = result_elf.read_bytes()
    if hashlib.sha256(prepared).hexdigest() != ORIGINAL_SHA256:
        raise ValueError('Prepared original ELF does not match supported user game')
    with tempfile.NamedTemporaryFile(prefix='verified-', suffix='.elf', dir=destination, delete=False) as stream:
        verified = Path(stream.name)
        stream.write(prepared)
    try:
        verified.replace(plain)
    finally:
        verified.unlink(missing_ok=True)
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
