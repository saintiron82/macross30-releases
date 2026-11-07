"""Read-only ISO9660 inventory and bounded extraction for Macross analysis."""
import argparse
import json
import struct
from pathlib import Path


def inventory(stream):
    stream.seek(16 * 2048)
    pvd = stream.read(2048)
    if pvd[:7] != b'\x01CD001\x01':
        raise ValueError('Expected ISO9660 primary volume descriptor')
    files = []
    visited = set()

    def walk(record, parent):
        extent = struct.unpack_from('<I', record, 2)[0]
        size = struct.unpack_from('<I', record, 10)[0]
        if extent in visited:
            return
        visited.add(extent)
        stream.seek(extent * 2048)
        data = stream.read(size)
        pos = 0
        while pos < len(data):
            length = data[pos]
            if not length:
                pos = (pos // 2048 + 1) * 2048
                continue
            rec = data[pos:pos + length]
            pos += length
            name = rec[33:33 + rec[32]]
            if name in (b'\x00', b'\x01'):
                continue
            name = name.decode('ascii', errors='replace').split(';')[0]
            path = f'{parent}/{name}'.lstrip('/')
            if rec[25] & 2:
                walk(rec, path)
            else:
                files.append({'path': path, 'offset': struct.unpack_from('<I', rec, 2)[0] * 2048,
                              'size': struct.unpack_from('<I', rec, 10)[0]})

    walk(pvd[156:190], '')
    merged = {}
    for item in files:
        span = {'offset': item['offset'], 'size': item['size']}
        if item['path'] in merged:
            merged[item['path']]['extents'].append(span)
            merged[item['path']]['size'] += item['size']
        else:
            merged[item['path']] = dict(item, extents=[span])
    return list(merged.values())


def read_file(stream, item, offset=0, size=None):
    size = item['size'] - offset if size is None else size
    if offset < 0 or size < 0 or offset + size > item['size']:
        raise ValueError('Read outside file')
    result = bytearray()
    for extent in item['extents']:
        if offset >= extent['size']:
            offset -= extent['size']
            continue
        amount = min(size, extent['size'] - offset)
        stream.seek(extent['offset'] + offset)
        result.extend(stream.read(amount))
        size -= amount
        offset = 0
        if not size:
            break
    return bytes(result)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('iso', type=Path)
    parser.add_argument('--output', type=Path, default=Path('analysis'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with args.iso.open('rb') as stream:
        files = inventory(stream)
        for item in files:
            stream.seek(item['offset'])
            item['header_hex'] = stream.read(min(32, item['size'])).hex()
            name = Path(item['path']).name
            if name in ('PARAM.SFO', 'EBOOT.BIN', 'PS3_DISC.SFB') or name.lower().endswith(('.idx', '.ttf', '.otf')):
                if item['size'] <= 64 * 1024 * 1024:
                    target = args.output / 'extracted' / item['path']
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(read_file(stream, item))
        (args.output / 'iso_inventory.json').write_text(json.dumps(files, ensure_ascii=False, indent=2), encoding='utf-8')
        print(f'{len(files)} files; inventory: {args.output / "iso_inventory.json"}')
        for item in files:
            print(f'{item["size"]:12d} {item["path"]} [{item["header_hex"][:16]}]')


if __name__ == '__main__':
    main()
