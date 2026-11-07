"""Export null-delimited UTF-8 Japanese candidates, not a GOP structure parser.

Offsets are provenance only. Do not use this catalog for variable-length binary
replacement until GOP pointer/record reconstruction has been implemented.
"""
import hashlib
import json
import re
import struct
from pathlib import Path


root = Path('analysis')
destination = Path('localization/ko-KR')
destination.mkdir(parents=True, exist_ok=True)
summary = {}
for source in sorted((root / 'gop').glob('*.gop')):
    data = source.read_bytes()
    records = []
    for match in re.finditer(rb'[^\x00]{3,}', data):
        try:
            text = match.group().decode('utf-8')
        except UnicodeDecodeError:
            continue
        if not any('\u3040' <= c <= '\u30ff' or '\u3400' <= c <= '\u9fff' for c in text):
            continue
        if any(ord(c) < 32 and c not in '\r\n\t' for c in text):
            continue
        records.append(dict(id=f'{source.stem}:{match.start():08x}', offset=match.start(),
                            source=text, translation='', status='candidate',
                            source_bytes=len(match.group())))
    if records:
        target = destination / (source.stem + '.json')
        if target.exists():
            previous = {r['id']: r for r in json.loads(target.read_text(encoding='utf-8'))['entries']}
            for row in records:
                old = previous.get(row['id'])
                if old and old['source'] == row['source']:
                    row['translation'] = old.get('translation', '')
                    row['status'] = old.get('status', 'candidate')
        target.write_text(json.dumps(dict(source_file=str(source), source_sha256=hashlib.sha256(data).hexdigest(),
                                         extraction='UTF-8 null-delimited candidates; structural validation pending',
                                         entries=records), ensure_ascii=False, indent=2), encoding='utf-8')
        summary[source.name] = len(records)

sfo = (root / 'extracted/PS3_GAME/PARAM.SFO').read_bytes()
_, version, keys, values, count = struct.unpack_from('<5I', sfo)
metadata = {}
for i in range(count):
    keyoff, fmt, length, capacity, valueoff = struct.unpack_from('<HHIII', sfo, 20 + i * 16)
    key = sfo[keys + keyoff:].split(b'\0')[0].decode('utf-8')
    value = sfo[values + valueoff:values + valueoff + length]
    metadata[key] = struct.unpack('<I', value)[0] if fmt == 0x404 else value.rstrip(b'\0').decode('utf-8', errors='replace')
(root / 'text_summary.json').write_text(json.dumps(dict(metadata=metadata, candidates=summary,
                                                       total=sum(summary.values())), ensure_ascii=False, indent=2), encoding='utf-8')
print(json.dumps(dict(metadata=metadata, candidates=summary, total=sum(summary.values())), ensure_ascii=True, indent=2))
