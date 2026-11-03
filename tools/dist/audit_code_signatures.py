"""Check a supplied PE for the known original comparison arrays, without exporting them."""
import argparse
import hashlib
import json
from pathlib import Path


def audit(exe, original_elf, findings):
    executable = Path(exe).read_bytes()
    original = Path(original_elf).read_bytes()
    known = json.loads(Path(findings).read_text(encoding='utf-8'))
    if executable[:2] != b'MZ':
        raise ValueError('Expected a Windows PE candidate')
    if hashlib.sha256(original).hexdigest() != known['original_elf_sha256']:
        raise ValueError('Original ELF identity mismatch')
    if not known.get('literal_arrays'):
        raise ValueError('No comparison-array evidence supplied')
    rows = []
    for item in known['literal_arrays']:
        offset, size = int(item['original_elf_offset_hex'], 16), item['bytes']
        if not isinstance(size, int) or size <= 0 or offset < 0 or offset + size > len(original):
            raise ValueError('Invalid original comparison span')
        needle = original[offset:offset + size]
        if hashlib.sha256(needle).hexdigest() != item['sha256']:
            raise ValueError('Comparison-array evidence hash mismatch: ' + item['symbol'])
        rows.append(dict(symbol=item['symbol'], bytes=size, sha256=item['sha256'],
                         present_in_exe=needle in executable))
    return dict(passed=not any(row['present_in_exe'] for row in rows),
                exe_sha256=hashlib.sha256(executable).hexdigest(),
                comparison_arrays_checked=len(rows), comparison_bytes_checked=sum(row['bytes'] for row in rows),
                scope='Only the explicitly evidenced comparison arrays; no exhaustive binary claim',
                arrays=rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--original-elf', type=Path, required=True)
    parser.add_argument('--findings', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = audit(args.exe, args.original_elf, args.findings)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({key:value for key,value in result.items() if key != 'arrays'}))
    raise SystemExit(0 if result['passed'] else 1)


if __name__ == '__main__':
    main()
