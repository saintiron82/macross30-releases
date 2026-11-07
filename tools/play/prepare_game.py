"""Extract a separate disc folder and apply bounded GOP/index changes there."""
import hashlib
import argparse
import json
import struct
from pathlib import Path
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from inspect_iso import inventory, read_file
from analyze_archives import cstring


def size_field(data):
    _, _, _, _, _, secondary, _, names, names_size = struct.unpack_from('<9I', data, 4)
    strings = data[names:names + names_size]
    count = struct.unpack_from('<I', data, secondary)[0]
    matches = []
    for i in range(count):
        rec = secondary + struct.unpack_from('<I', data, secondary + 4 + i * 4)[0]
        no, pn, offset, size = struct.unpack_from('<4I', data, rec)
        if cstring(strings, no) == 'gop':
            assert cstring(strings, pn) == 'data/pack/data.dat'
            matches.append((rec + 12, offset, size))
    assert len(matches) == 1
    return matches[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--destination', type=Path, default=Path('build/game'))
    args = parser.parse_args()
    iso = next(Path('.').glob('*/*.iso'))
    destination = args.destination.resolve()
    workspace = Path('.').resolve()
    if not destination.is_relative_to(workspace / 'build'):
        raise ValueError('Destination must be inside the workspace build directory')
    if destination == (workspace / 'build/game') and Path('analysis/rpcs3_boot.pid').exists():
        import subprocess
        pid = Path('analysis/rpcs3_boot.pid').read_text().strip()
        if pid.isdigit():
            status = subprocess.run(['tasklist', '/FI', f'PID eq {pid}', '/FO', 'CSV', '/NH'], capture_output=True)
            if b'rpcs3.exe' in status.stdout.lower():
                raise RuntimeError('Active RPCS3 test may be reading build/game; use a separate destination')
    marker = destination / '.extraction_complete.json'
    with iso.open('rb') as stream:
        files = inventory(stream)
        selected = [f for f in files if not f['path'].startswith('PS3_UPDATE/')]
        if not marker.exists():
            for i, item in enumerate(selected):
                target = destination / item['path']
                target.parent.mkdir(parents=True, exist_ok=True)
                with target.open('wb') as output:
                    for offset in range(0, item['size'], 8 * 1024 * 1024):
                        output.write(read_file(stream, item, offset, min(8 * 1024 * 1024, item['size'] - offset)))
                if i % 30 == 0:
                    print('Extracted', i + 1, '/', len(selected), flush=True)
            marker.write_text(json.dumps(dict(iso_size=iso.stat().st_size, files=len(selected))), encoding='utf-8')
        idx_item = next(f for f in files if f['path'].endswith('/PACK.IDX'))
        pack_item = next(f for f in files if f['path'].endswith('/DATA.DAT'))
        source_idx = read_file(stream, idx_item)
        source_header = read_file(stream, pack_item, 0, 200000)
        idx_field, archive_offset, old_size = size_field(source_idx)
        inner_field, inner_offset, inner_size = size_field(source_header)
        assert (inner_offset, inner_size) == (archive_offset, old_size)
        old_archive = read_file(stream, pack_item, archive_offset, old_size)
        assert old_archive == Path('analysis/gop_archive.fsts').read_bytes()
        replacement = Path('build/patch/gop_archive.fsts').read_bytes()
        growth = len(replacement) - old_size
        assert len(replacement) > 0 and growth <= 1088
        assert read_file(stream, pack_item, archive_offset + old_size, 1088) == b'\0' * 1088
        idx = bytearray(source_idx)
        struct.pack_into('<I', idx, idx_field, len(replacement))
        assert size_field(idx)[2] == len(replacement)
        (destination / idx_item['path']).write_bytes(idx)
        with (destination / pack_item['path']).open('r+b') as output:
            output.seek(archive_offset)
            output.write(replacement.ljust(old_size + 1088, b'\0'))
            output.seek(inner_field)
            output.write(struct.pack('<I', len(replacement)))
            output.seek(archive_offset)
            assert output.read(len(replacement)) == replacement
            output.seek(0)
            assert size_field(output.read(200000))[2] == len(replacement)
        assert (destination / pack_item['path']).stat().st_size == pack_item['size']
        result = dict(destination=str(destination), files=len(selected), original_iso_modified=False, patch_growth=growth,
                      archive_offset=archive_offset, pack_idx_size_field=idx_field,
                      data_dat_size_field=inner_field, runtime_verified=False,
                      archive_sha256=hashlib.sha256(replacement).hexdigest())
        Path('analysis/game_build_validation.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        print(json.dumps(result, indent=2), flush=True)


if __name__ == '__main__':
    main()
