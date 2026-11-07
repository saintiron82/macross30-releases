"""Check original GOP strings beyond the Korean catalog selection."""
import json
import re
from english_pack import ROOT,digest,write
from gop_strings import parse

if __name__=='__main__':
    catalogs={p.stem:{r['index']:r for r in json.loads(p.read_text(encoding='utf8'))['entries']} for p in (ROOT/'localization/en-US/structured').glob('*.json')}
    japanese=re.compile(r'[\u3040-\u30ff\u3400-\u9fff]');files=[];missing=[];metadata=[]
    for p in sorted((ROOT/'analysis/gop').glob('*.gop')):
        raw=p.read_bytes();rows=parse(raw);count=0
        for r in rows:
            if not japanese.search(r['text']):continue
            if r['metadata']:
                metadata.append(dict(file=p.name,index=r['index'],text=r['text'],classification='GOP_TYPE schema/metadata; not display text'));continue
            count+=1;entry=catalogs.get(p.stem,{}).get(r['index'])
            if entry is None or entry['source']!=r['text'] or not entry['translation']:
                missing.append(dict(file=p.name,index=r['index'],source=r['text']))
        files.append(dict(file=p.name,source_sha256=digest(raw),display_Japanese_entries=count))
    report=dict(status='PASS' if not missing else 'FAIL',files=files,missing=missing,excluded_metadata=metadata,
                scope='All extracted original GENESTRT GOP catalogs, including those absent from the Korean translation set',
                limitation='Does not classify arbitrary ELF debug/internal strings or text painted into uninspected 3D model textures')
    write(ROOT/'localization/en-US/review-v10/source-gop-scope.json',report)
    print(dict(original_GOP_files=len(files),missing=len(missing)))
    if missing:raise ValueError('Original display strings missing from English scope')
