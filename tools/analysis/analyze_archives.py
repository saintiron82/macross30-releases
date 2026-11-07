"""Inspect PIDX/FSTS archives directly from an ISO; never modify the source.

Format reference: Luigi Auriemma's macross_pidx/macross_fsts QuickBMS scripts,
and alanm20/Macross30text's offset2 description. Independently implemented.
"""
import json
import struct
import zlib
from pathlib import Path
import sys as _sys, pathlib as _pl
if not getattr(_sys, "frozen", False):
    _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]
from inspect_iso import inventory, read_file


def cstring(data, offset):
    return data[offset:data.index(b'\0', offset)].decode('utf-8')


def unpack(data, expected):
    if data[:4] != b'segs':
        result = zlib.decompress(data)
    else:
        _, count, full, packed = struct.unpack_from('>HHII', data, 4)
        result = bytearray()
        relative = struct.unpack_from('>I', data, 20)[0] == 1
        for i in range(count):
            zs, size, off = struct.unpack_from('>HHI', data, 16 + i * 8)
            size = size or 65536
            off = off - 1 + (16 + count * 8 if relative else 0)
            chunk = data[off:off + zs]
            result.extend(chunk if zs == size else zlib.decompress(chunk, -15))
        assert len(result) == full
    assert len(result) == expected, (len(result), expected)
    return bytes(result)


def main():
    out = Path('analysis')
    iso = next(Path('.').glob('*/*.iso'))
    with iso.open('rb') as stream:
        files = inventory(stream)
        idx_item = next(i for i in files if i['path'].endswith('/PACK.IDX'))
        idx = read_file(stream, idx_item)
        po, pc, info, count, flags, secondary, secondary_size, names, names_size = struct.unpack_from('<9I', idx, 4)
        strings = idx[names:names + names_size]
        rows = []
        for i in range(count):
            folder, no, pn, off, size, zs = struct.unpack_from('<6I', idx, info + i * 24)
            if not folder:
                rows.append(dict(path=cstring(strings, no), pack=cstring(strings, pn), offset=off, size=size, compressed_size=zs))
        (out / 'pack_inventory.json').write_text(json.dumps(rows, ensure_ascii=False, indent=2), encoding='utf-8')
        n = struct.unpack_from('<I', idx, secondary)[0]
        secondary_rows = []
        for i in range(n):
            p = secondary + struct.unpack_from('<I', idx, secondary + 4 + i * 4)[0]
            no, pn, off, size = struct.unpack_from('<4I', idx, p)
            secondary_rows.append(dict(path=cstring(strings, no), pack=cstring(strings, pn), offset=off, size=size))
        (out / 'pack_archives.json').write_text(json.dumps(secondary_rows, indent=2), encoding='utf-8')
        for row in secondary_rows:
            if Path(row['path']).name.lower() != 'gop':
                continue
            pack = next(f for f in files if Path(f['path']).name.lower() == Path(row['pack']).name.lower())
            blob = read_file(stream, pack, row['offset'], row['size'])
            assert blob[:4] == b'FSTS', blob[:16]
            num, table, ns, nsz = struct.unpack_from('<4I', blob, 4)
            for i in range(num):
                no, off, size, zs = struct.unpack_from('<4I', blob, table + i * 16)
                name = cstring(blob[ns:ns + nsz], no)
                data = blob[off:off + (zs if 0 < zs < size else size)]
                if 0 < zs < size:
                    data = unpack(data, size)
                target = out / 'gop' / Path(name).name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            print('GOP archive:', row, 'entries:', num)
        print('Indexed files:', len(rows), 'archives:', n)
        print('Font candidates:', [r['path'] for r in rows if any(x in r['path'].lower() for x in ('font', '.ttf', '.otf', '.fnt'))])


if __name__ == '__main__':
    main()
