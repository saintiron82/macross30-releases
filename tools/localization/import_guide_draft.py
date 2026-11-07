import json
import sys
from pathlib import Path

stem = sys.argv[1] if len(sys.argv) > 1 else 'guide'
path = Path(f'localization/ko-KR/structured/gop_{stem}.json')
doc = json.loads(path.read_text(encoding='utf-8'))
draft = {}
for line in Path(f'localization/ko-KR/{stem}_draft.tsv').read_text(encoding='utf-8').splitlines():
    index, text = line.split('\t', 1)
    assert int(index) not in draft
    draft[int(index)] = text
assert set(draft) == {r['index'] for r in doc['entries']}
for row in doc['entries']:
    row['translation'] = draft[row['index']]
    row['status'] = 'Korean draft; terminology/font/layout/runtime review pending'
path.write_text(json.dumps(doc, ensure_ascii=False, indent=2), encoding='utf-8')
print('Korean', stem, 'draft entries:', len(draft))
