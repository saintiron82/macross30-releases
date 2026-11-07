"""Extract selected primary PIDX assets into analysis, leaving the ISO intact."""
import argparse
import json
from pathlib import Path
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from inspect_iso import inventory, read_file
from analyze_archives import unpack


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('names', nargs='+')
    args = parser.parse_args()
    names = set(args.names)
    rows = json.loads(Path('analysis/pack_inventory.json').read_text())
    found = set()
    with next(Path('.').glob('*/*.iso')).open('rb') as stream:
        packs = {Path(r['path']).name.lower(): r for r in inventory(stream)}
        for row in rows:
            if row['path'] not in names:
                continue
            found.add(row['path'])
            zs, size = row['compressed_size'], row['size']
            pack = packs[Path(row['pack']).name.lower()]
            data = read_file(stream, pack, row['offset'], zs if 0 < zs < size else size)
            if 0 < zs < size:
                data = unpack(data, size)
            target = Path('analysis/assets') / Path(row['pack']).stem / Path(row['path']).name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            print(target, len(data), data[:24].hex())
    if names - found:
        raise ValueError(f'Assets not found: {names - found}')


if __name__ == '__main__':
    main()
