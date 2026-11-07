"""Bounded GENESTRT reader and offset-table rebuilder. No source mutation."""
import hashlib
import json
import re
import struct
from pathlib import Path


def parse(data):
    if data[:8] != b'ENDIBIGE':
        raise ValueError('Not a big-endian GOP')
    pos = 16
    while pos + 16 <= len(data):
        tag = data[pos:pos + 8]
        size = struct.unpack_from('>I', data, pos + 8)[0]
        end = pos + 16 + size
        if end > len(data):
            raise ValueError('Section outside file')
        if tag == b'GENESTRT':
            base = pos + 16
            count, table, values, limit = struct.unpack_from('>4I', data, base)
            assert table >= 16 and table + 4 * count <= values <= size
            offsets = struct.unpack_from(f'>{count}I', data, base + table)
            assert list(offsets) == sorted(offsets)
            entries = []
            metadata = False
            for i, offset in enumerate(offsets):
                start = base + values + offset
                stop = base + values + offsets[i + 1] if i + 1 < count else end
                assert base + values <= start < stop <= end
                terminator = data.index(b'\0', start, stop)
                text = data[start:terminator].decode('utf-8')
                metadata |= text.startswith('GOP_TYPE')
                entries.append(dict(index=i, offset=start, capacity=stop-start,
                                    text=text, metadata=metadata))
            return entries
        pos = end
    raise ValueError('GENESTRT missing')


TOKEN = re.compile(r'%%|%[-+ #0]*\d*(?:\.\d+)?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcspn]|\\[nrt]|\[[^\]]+\]')


def patch(data, translations):
    rows = {r['offset']: r for r in parse(data)}
    output = bytearray(data)
    changed = []
    replacements = {}
    for item in translations:
        if not item.get('translation'):
            continue
        row = rows[item['offset']]
        if row['metadata'] or row['text'] != item['source']:
            raise ValueError('Source mismatch or metadata replacement')
        if TOKEN.findall(row['text']) != TOKEN.findall(item['translation']):
            raise ValueError('Control tokens differ')
        encoded = item['translation'].encode('utf-8') + b'\0'
        replacements[row['index']] = encoded
        changed.append(row['index'])
    if changed:
        section = data.index(b'GENESTRT')
        base = section + 16
        _, table, values, _ = struct.unpack_from('>4I', data, base)
        start = base + values
        end = base + struct.unpack_from('>I', data, section + 8)[0]
        cursor = start
        output[start:end] = b'\0' * (end - start)
        for row in rows.values():
            encoded = replacements.get(row['index'], row['text'].encode('utf-8') + b'\0')
            if cursor + len(encoded) > end:
                raise ValueError('Translations exceed original string section capacity')
            struct.pack_into('>I', output, base + table + 4 * row['index'], cursor - start)
            output[cursor:cursor + len(encoded)] = encoded
            cursor += len(encoded)
    reparsed = parse(output)
    assert len(reparsed) == len(rows)
    for before, after in zip(rows.values(), reparsed):
        if before['index'] not in changed:
            assert before['text'] == after['text']
        else:
            assert after['text'].encode('utf-8') + b'\0' == replacements[before['index']]
    return bytes(output), changed


def main():
    report = []
    for file in sorted(Path('analysis/gop').glob('*.gop')):
        data = file.read_bytes()
        rows = parse(data)
        assert patch(data, [])[0] == data
        report.append(dict(file=file.name, strings=len(rows), metadata=sum(r['metadata'] for r in rows)))
    patched = []
    for catalog_file in sorted(Path('localization/ko-KR/structured').glob('*.json')):
        catalog = json.loads(catalog_file.read_text(encoding='utf-8'))
        if not any(r.get('translation') for r in catalog['entries']):
            continue
        source = Path(catalog['source_file'])
        data = source.read_bytes()
        assert hashlib.sha256(data).hexdigest() == catalog['source_sha256']
        output, changed = patch(data, catalog['entries'])
        target = Path('build/patch') / source.name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(output)
        patched.append(dict(file=source.name, indices=changed, sha256=hashlib.sha256(output).hexdigest()))
    result = dict(files=report, roundtrip_files=len(report), patched=patched, runtime_verified=False)
    Path('analysis/gop_validation.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print('Byte-identical empty roundtrips:', len(report))
    print('Korean replacements with rebuilt offset tables:', sum(len(r['indices']) for r in patched))


if __name__ == '__main__':
    main()
