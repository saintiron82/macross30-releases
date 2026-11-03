"""Read-only local ISO diagnostics. Never classify four random bytes as proof of encryption."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'play'))
from inspect_iso import inventory, read_file


def inspect(path, hashes=False):
    path = Path(path).resolve(strict=True)
    with path.open('rb') as stream:
        files = inventory(stream)
        entry = next((r for r in files if r['path'].casefold() == 'ps3_game/usrdir/eboot.bin'), None)
        if entry is None:
            raise ValueError('PS3_GAME/USRDIR/EBOOT.BIN not found')
        header = read_file(stream, entry, 0, min(4, entry['size']))
    kind = {b'\x7fELF': 'elf', b'SCE\0': 'sce-self'}.get(header, 'unrecognized')
    result = dict(path=str(path), bytes=path.stat().st_size, eboot_bytes=entry['size'],
                  eboot_magic=header.hex(), eboot_format=kind,
                  installer_header_supported=kind != 'unrecognized',
                  explanation=('Header recognized; full original ELF identity still needs verification.'
                               if kind != 'unrecognized' else
                               'Disc-layer encryption, an unsupported image, or damaged data is possible.'))
    if hashes:
        sha1, md5, sha256 = hashlib.sha1(), hashlib.md5(), hashlib.sha256()
        with path.open('rb') as stream:
            while data := stream.read(8 << 20):
                sha1.update(data); md5.update(data); sha256.update(data)
        result['hashes'] = dict(sha1=sha1.hexdigest(), md5=md5.hexdigest(), sha256=sha256.hexdigest())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('iso', type=Path)
    parser.add_argument('--hashes', action='store_true')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    result = inspect(args.iso, args.hashes)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
