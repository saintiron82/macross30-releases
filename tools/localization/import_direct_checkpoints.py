"""Integrate explicitly authored checkpoints; never generate translations."""
from pathlib import Path
import json
import bilingual_review as review
from language_pack import read,write
ROOT=Path(__file__).resolve().parents[2]
BASE=ROOT/'localization/ja-en-ru-review'


def main():
    admitted=0
    for p in sorted((BASE/'authored').glob('*.json')):
        d=read(p)
        if not d.get('catalog','').startswith('gop_') or not d.get('translations_es'):continue
        if not (BASE/'reviewed-es'/p.name).exists():review.spanish(p);admitted+=1
    counts={}
    for locale,field,folder in [('ru-RU','ru','reviewed'),('es-ES','es','reviewed-es')]:
        p=ROOT/'localization'/locale/'authoring-project-v1/project.json'
        d=read(p)
        for batch in (BASE/folder).glob('*.json'):
            for row in read(batch)['entries']:d['entries'][row['id']]['text']=row[field]
        for batch in (BASE/'authored').glob('elf-catalog-*.json'):
            b=read(batch)
            for offset,text in b['translations' if locale=='ru-RU' else 'translations_es'].items():
                d['entries']['elf:string:'+offset]['text']=text
        variants=BASE/'authored/elf-display-variants.json'
        if variants.exists():
            from gop_strings import TOKEN
            for row in read(variants)['entries'].values():
                if field not in row:continue
                source=d['entries'][row['id']]
                assert source['source_sha256']==row['source_sha256'] and source['source']==row['source_ja']
                assert TOKEN.findall(row[field])==TOKEN.findall(source['source'])
                assert len(row[field].encode())+1<=row['capacity']
                source['text']=row[field];source['display_variant_note']=row['reason']
        ui=BASE/'authored/native-ui-001.json'
        if ui.exists():d['native_ui']=read(ui)['translations' if locale=='ru-RU' else 'translations_es']
        write(p,d)
        counts[locale]=dict(authored_entries=sum(bool(r['text']) for r in d['entries'].values()),native_ui=len(d['native_ui']))
    print(json.dumps(dict(new_checkpoints=admitted,projects=counts)))
if __name__=='__main__':main()
