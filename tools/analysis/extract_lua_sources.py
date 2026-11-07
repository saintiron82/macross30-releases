"""Read original LUA.DAT entries into analysis for native-script debugging."""
import collections
import hashlib
import json
from pathlib import Path
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from analyze_archives import unpack


def main():
    root = Path(__file__).resolve().parents[2]
    rows = json.loads((root / 'analysis/pack_inventory.json').read_text(encoding='utf-8'))
    rows = [r for r in rows if r['pack'].lower() == 'data/pack/lua.dat'
            and r['path'].lower().endswith('.lua')]
    names = collections.Counter(Path(r['path']).name for r in rows)
    source = root / 'native/vfs/PS3_GAME/USRDIR/DATA/PACK/LUA.DAT'
    output = root / 'analysis/scripts'
    output.mkdir(exist_ok=True)
    records = []
    with source.open('rb') as stream:
        length = source.stat().st_size
        for row in rows:
            size = row['size']
            packed = row['compressed_size']
            stored = packed if 0 < packed < size else size
            if row['offset'] < 0 or stored < 0 or row['offset'] + stored > length:
                raise ValueError('Lua entry outside original pack')
            stream.seek(row['offset'])
            encoded = stream.read(stored)
            data = unpack(encoded, size) if stored < size else encoded
            if len(data) != size:
                raise ValueError('Short Lua entry')
            name = Path(row['path']).name
            if names[name] != 1:
                name = f"{row['offset']:08x}-{name}"
            destination = output / name
            if destination.exists() and destination.read_bytes() != data:
                raise ValueError(f'Refusing to replace differing analysis file {name}')
            destination.write_bytes(data)
            records.append(dict(source_entry=row, output=str(destination.relative_to(root)),
                                sha256=hashlib.sha256(data).hexdigest(),
                                bytecode=data.startswith(b'\x1bLua')))
    (output / 'lua-pack-manifest.json').write_text(json.dumps(
        dict(source=str(source), original_writes=False, entries=records), indent=2), encoding='utf-8')
    print(f'Extracted {len(records)} original Lua entries for read-only analysis.')


if __name__ == '__main__':
    main()
