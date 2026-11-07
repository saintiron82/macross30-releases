"""Compare all extracted bytes to source, allowing only explicit patch ranges."""
import hashlib
import json
import struct
from pathlib import Path
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from inspect_iso import inventory, read_file
from prepare_game import size_field
from gop_strings import parse, patch


def main():
    replacement = Path('build/patch/gop_archive.fsts').read_bytes()
    validation = json.loads(Path('analysis/game_build_validation.json').read_text())
    modified = {}
    total = 0
    files_verified = 0
    with next(Path('.').glob('*/*.iso')).open('rb') as source:
        for item in inventory(source):
            if item['path'].startswith('PS3_UPDATE/'):
                continue
            path = Path(validation.get('destination', 'build/game')) / item['path']
            assert path.stat().st_size == item['size'], path
            digest = hashlib.sha256()
            with path.open('rb') as target:
                for offset in range(0, item['size'], 8 * 1024 * 1024):
                    size = min(8 * 1024 * 1024, item['size'] - offset)
                    expected = bytearray(read_file(source, item, offset, size))
                    ranges = []
                    if item['path'].endswith('/PACK.IDX'):
                        ranges.append((validation['pack_idx_size_field'], struct.pack('<I', len(replacement))))
                    if item['path'].endswith('/DATA.DAT'):
                        ranges.extend([(validation['data_dat_size_field'], struct.pack('<I', len(replacement))),
                                       (validation['archive_offset'], replacement.ljust(953280 + 1088, b'\0'))])
                    for start, payload in ranges:
                        low, high = max(offset, start), min(offset + size, start + len(payload))
                        if low < high:
                            expected[low-offset:high-offset] = payload[low-start:high-start]
                    actual = target.read(size)
                    assert actual == expected, (item['path'], offset)
                    digest.update(actual)
                    total += size
            files_verified += 1
            if ranges:
                modified[item['path']] = digest.hexdigest()
    guide = Path('analysis/gop/gop_guide.gop').read_bytes()
    rows = parse(guide)
    first = next(r for r in rows if r['text'] == '難易度選択')
    rejected = []
    for label, row in [('source mismatch', dict(offset=first['offset'], source='wrong', translation='테스트')),
                       ('capacity overflow', dict(offset=first['offset'], source=first['text'], translation='가' * len(guide)))]:
        try:
            patch(guide, [row])
        except ValueError:
            rejected.append(label)
        else:
            raise AssertionError(label)
    token = next(r for r in parse(Path('analysis/gop/gop_systemtext.gop').read_bytes()) if '%s' in r['text'] and not r['metadata'])
    try:
        patch(Path('analysis/gop/gop_systemtext.gop').read_bytes(),
              [dict(offset=token['offset'], source=token['text'], translation='토큰 누락')])
    except ValueError:
        rejected.append('control token mismatch')
    else:
        raise AssertionError('control token mismatch')
    result = dict(destination=validation.get('destination', 'build/game'), files_verified=files_verified, bytes_compared=total, allowed_modified_files=modified,
                  negative_cases_rejected=rejected, runtime_verified=False)
    Path('analysis/full_game_validation.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
