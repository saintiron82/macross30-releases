"""Export actual pre-metadata GENESTRT entries, preserving prior translations."""
import hashlib
import json
from pathlib import Path
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from gop_strings import parse


out = Path('localization/ko-KR/structured')
out.mkdir(parents=True, exist_ok=True)
summary = {}
for source in sorted(Path('analysis/gop').glob('*.gop')):
    data = source.read_bytes()
    entries = []
    old_file = Path('localization/ko-KR') / (source.stem + '.json')
    old = {r['offset']: r for r in json.loads(old_file.read_text(encoding='utf-8'))['entries']} if old_file.exists() else {}
    target = out / (source.stem + '.json')
    previous = {r['index']: r for r in json.loads(target.read_text(encoding='utf-8'))['entries']} if target.exists() else {}
    for row in parse(data):
        if row['metadata'] or not any('\u3040' <= c <= '\u30ff' or '\u3400' <= c <= '\u9fff' for c in row['text']):
            continue
        prior = previous.get(row['index']) or old.get(row['offset'], {})
        if prior.get('source') != row['text']:
            prior = {}
        entries.append(dict(id=f'{source.stem}:string:{row["index"]}', index=row['index'], offset=row['offset'],
                            source=row['text'], translation=prior.get('translation', ''),
                            status=prior.get('status', 'untranslated; runtime role pending')))
    if entries:
        doc = dict(source_file=str(source), source_sha256=hashlib.sha256(data).hexdigest(),
                   extraction='GENESTRT string table; entries after GOP_TYPE excluded', entries=entries)
        target.write_text(json.dumps(doc, ensure_ascii=False, indent=2), encoding='utf-8')
        summary[source.name] = len(entries)
Path('analysis/structured_text_summary.json').write_text(json.dumps(dict(files=summary, total=sum(summary.values())), indent=2), encoding='utf-8')
print('Structured translation candidates:', sum(summary.values()), 'in', len(summary), 'GOPs')
