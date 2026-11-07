"""Flag likely name loss and unusually short drafts for human comparison.

Heuristics only: an empty flag list is not proof of correct translation.
"""
import json
import re
from english_pack import ROOT,write

if __name__=='__main__':
    terms=json.loads((ROOT/'tools/localization/catalogs/english-domain-terms.json').read_text(encoding='utf8'))
    ignore={'統合軍','新統合軍','ファイター','僚機','機体','行動不能','実体弾','バンデット'}
    flags=[]
    for path in sorted((ROOT/'localization/en-US/structured').glob('*.json')):
        doc=json.loads(path.read_text(encoding='utf8'))
        for row in doc['entries']:
            if row.get('status')!='local_model_draft_needs_review':continue
            text=' '.join(row['translation'].replace(chr(92)+'n',' ').split());reasons=[]
            n=len(re.findall(r'[\u3040-\u30ff\u3400-\u9fff]',row['source']))
            if n>25 and len(text.split())<n/3.6:reasons.append('unusually_short')
            for source,target in terms.items():
                if source in ignore or len(source)<=2:continue
                if source in row['source'] and target.lower() not in text.lower():reasons.append('check_term:'+target)
            if reasons:flags.append(dict(id=row['id'],catalog=path.stem,index=row['index'],source=row['source'],translation=row['translation'],reasons=reasons))
    write(ROOT/'localization/en-US/build/semantic-review-flags.json',dict(flags=flags,heuristics_only=True,full_semantic_review=False))
    print(json.dumps(dict(flagged=len(flags),full_semantic_review=False)))
