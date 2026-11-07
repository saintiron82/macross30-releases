"""Read original GOP records, joining voice keys to existing Korean translations.

Offline evidence only: no asset installation or runtime subtitle injection.
"""
import hashlib
import json
from pathlib import Path
import re
import struct

from gop_strings import parse

ROOT = Path(__file__).resolve().parents[2]


def main():
    source = ROOT / 'analysis/gop/gop_battletext.gop'
    data = source.read_bytes()
    strings = parse(data)
    catalog = json.loads((ROOT / 'localization/ko-KR/structured/gop_battletext.json').read_text(encoding='utf-8'))
    assert hashlib.sha256(data).hexdigest() == catalog['source_sha256']
    translations = {e['index']: e for e in catalog['entries']}
    for index, item in translations.items():
        assert strings[index]['text'] == item['source']
    pos = 16
    section = None
    while pos + 16 <= len(data):
        size = struct.unpack_from('>I', data, pos + 8)[0]
        assert pos + 16 + size <= len(data)
        if data[pos:pos+8] == b'GOP GDAT':
            section = data[pos+16:pos+16+size]
            break
        pos += 16 + size
    assert section is not None
    stride, count, lookup, records = struct.unpack_from('>4I', section)
    assert stride == 20 and lookup >= 16
    assert lookup + count * 4 <= records
    assert records + count * stride <= len(section)
    rows = []
    seen = set()
    patterns = {
        'worry_or_rescue_candidate': r'大丈夫|心配|しっかり|無茶|脱出|助け|援護|無事',
        'failure_or_loss_candidate': r'失敗|やられ|撃墜|撤退|離脱|帰還|落とされ',
        'damage_candidate': r'ダメージ|被弾|痛|損傷|もたない|持たない|直撃',
    }
    for number in range(count):
        rid, text, voice, character, face = struct.unpack_from('>5I', section, records + number*stride)
        assert max(rid, text, voice) < len(strings)
        record_id = strings[rid]['text']
        assert record_id.startswith('R_BattleText') and record_id not in seen
        seen.add(record_id)
        original = strings[text]['text']
        ko = translations.get(text)
        rows.append(dict(record_number=number, record_id=record_id,
                         text_string_index=text, voice_key=strings[voice]['text'],
                         character_enum=character, face_enum=face,
                         source=original, translation=ko['translation'] if ko else None,
                         categories=[name for name, pattern in patterns.items() if re.search(pattern, original)]))
    lookup_ids = struct.unpack_from(f'>{count}I', section, lookup)
    assert len(set(lookup_ids)) == count
    assert set(lookup_ids) == set(range(count))
    out = ROOT / 'localization/subtitles/battle-voice'
    out.mkdir(parents=True, exist_ok=True)
    report = dict(status='PASS', source=str(source.relative_to(ROOT)),
                  source_sha256=hashlib.sha256(data).hexdigest(),
                  record_count=count, translated_records=sum(bool(r['translation']) for r in rows),
                  nonempty_text_records=sum(bool(r['source']) for r in rows),
                  voiced_records=sum(bool(r['voice_key']) for r in rows),
                  candidate_counts={k: sum(k in r['categories'] for r in rows) for k in patterns},
                  limits=['Categories are lexical candidates, not proven gameplay triggers.',
                          'Voice keys are not globally unique; retain full record and character context.',
                          'No runtime voice-start/stop or subtitle visibility verified.',
                          'Existing Korean translations are drafts.'], records=rows)
    target = out / 'record-voice-text-map.json'
    target.write_bytes((json.dumps(report, ensure_ascii=False, indent=2)+'\n').encode('utf-8'))
    print(json.dumps({k:v for k,v in report.items() if k != 'records'}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
