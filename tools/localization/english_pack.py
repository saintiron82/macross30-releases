"""Export and build isolated English GOP drafts using the Korean-tested GOP route.

python tools/localization/english_pack.py
Never installs drafts or changes the active language selection.
"""
import hashlib
import json
import argparse
import re
from pathlib import Path

from gop_strings import TOKEN, parse, patch

ROOT = Path(__file__).resolve().parents[2]

def preview_version(value):
    if not re.fullmatch(r'v[1-9][0-9]*',value):raise argparse.ArgumentTypeError('Expected a version such as v8')
    return value


def digest(data):
    return hashlib.sha256(data).hexdigest()


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--include-model-drafts',action='store_true',help='Admit source/token/language-checked local English drafts; keep review provenance')
    args=parser.parse_args()
    base = ROOT / 'localization/en-US'
    seed = json.loads((base / 'catalogs/core-ui-v1.json').read_text(encoding='utf-8'))
    seeds = {r['id']: r for r in seed['entries']}
    battle = json.loads((base / 'catalogs/battle-rion-v1.json').read_text(encoding='utf-8'))
    battle_source = ROOT / battle['source_file']
    if digest(battle_source.read_bytes()) != battle['source_sha256']:
        raise ValueError('Battle translation source hash mismatch')
    battle_rows = {r['index']: r for r in parse(battle_source.read_bytes())}
    for n, text in enumerate(battle['translations']):
        index = battle['first_index'] + n * battle['index_step']
        seeds[f'gop_battletext:string:{index}'] = dict(
            source_ja=battle_rows[index]['text'], translation_en=text,
            source=dict(file_sha256=battle['source_sha256']))
    supplemental = json.loads((base / 'catalogs/guide-completion.json').read_text(encoding='utf8'))
    supplemental['gop_mainquest']=json.loads((base/'catalogs/mainquest.json').read_text(encoding='utf8'))
    completion=json.loads((ROOT/'tools/localization/catalogs/basic-text-completion.json').read_text(encoding='utf8'))
    for group,translations in completion.items():
        supplemental.setdefault(group,{}).update(translations)
    from english_item_text import translate as translate_item
    item_data=(ROOT/'analysis/gop/gop_item.gop').read_bytes()
    for row in parse(item_data):
        if row['metadata']:continue
        text=translate_item(row['index'],row['text'])
        if text:supplemental.setdefault('gop_item',{})[str(row['index'])]=text
    supplemental['gop_item'].update(json.loads((ROOT/'tools/localization/catalogs/item-details-completion.json').read_text(encoding='utf8')))
    tablets=json.loads((ROOT/'tools/localization/catalogs/stone-tablets.json').read_text(encoding='utf8'))
    assert len(tablets)==100
    item_original={r['index']:r['text'] for r in parse(item_data)}
    for n,text in enumerate(tablets):
        index=1951+3*n
        source=item_original[index]
        assert source.startswith('謎の一文が記されている。')
        # Retain the exact source line-break token count while keeping words intact.
        words=('A mysterious inscription. '+text).split()
        count=source.count(chr(92)+'n')+1
        lines=[]
        for line in range(count):
            take=round(len(words)/(count-line))
            lines.append(' '.join(words[:take]));words=words[take:]
        supplemental['gop_item'][str(index)]=(chr(92)+'n').join(lines)
    supplemental['gop_systemtext'].update({'329':'Refined classic PS3 Macross controls.',
        '331':'Controls similar to a standard third-person shooter.',
        '333':'Camera-focused aiming controls for this game.'})
    tutorial=json.loads((ROOT/'tools/localization/catalogs/scenario-tutorial-completion.json').read_text(encoding='utf8'))
    scenario_scope=json.loads((ROOT/'localization/ko-KR/structured/gop_scenariotext.json').read_text(encoding='utf8'))
    tutorial_rows=scenario_scope['entries'][75:255]
    assert len(tutorial)==len(tutorial_rows)==180
    assert tutorial_rows[0]['index']==232 and tutorial_rows[-1]['index']==772
    for row,text in zip(tutorial_rows,tutorial):
        words=text.split();count=row['source'].count(chr(92)+'n')+1;lines=[]
        for line in range(count):
            take=round(len(words)/(count-line));lines.append(' '.join(words[:take]));words=words[take:]
        supplemental.setdefault('gop_scenariotext',{})[str(row['index'])]=(chr(92)+'n').join(lines)
    for batch_path in sorted((ROOT/'tools/localization/catalogs/text-batches').glob('*.json')):
        batch=json.loads(batch_path.read_text(encoding='utf8'))
        group=batch['catalog'];raw=(ROOT/'analysis/gop'/(group+'.gop')).read_bytes()
        if digest(raw)!=batch['source_sha256']:raise ValueError('Stale translation batch: '+str(batch_path))
        originals={r['index']:r for r in parse(raw)}
        for key,text in batch['translations'].items():
            source=originals[int(key)]
            if source['metadata']:raise ValueError('Cannot translate metadata')
            if batch.get('preserve_line_count'):
                words=text.split();count=source['text'].count(chr(92)+'n')+1;lines=[]
                for line in range(count):
                    take=round(len(words)/(count-line));lines.append(' '.join(words[:take]));words=words[take:]
                text=(chr(92)+'n').join(lines)
            supplemental.setdefault(group,{})[key]=text
    for group, translations in supplemental.items():
        data = (ROOT / 'analysis/gop' / (group + '.gop')).read_bytes()
        original = {r['index']: r for r in parse(data)}
        for index, text in translations.items():
            text=text.replace(chr(10),chr(92)+'n')
            seeds[f'{group}:string:{index}'] = dict(source_ja=original[int(index)]['text'],
                translation_en=text, source=dict(file_sha256=digest(data)))
    scenario=json.loads((base/'catalogs/scenario-opening.json').read_text(encoding='utf8'))
    data=(ROOT/scenario['source_file']).read_bytes()
    if digest(data)!=scenario['source_sha256']:
        raise ValueError('Scenario source hash mismatch')
    original={r['index']:r for r in parse(data)}
    for n,text in enumerate(scenario['translations']):
        text=text.replace(chr(10),chr(92)+'n')
        index=scenario['first_index']+n*scenario['index_step']
        seeds[f'gop_scenariotext:string:{index}']=dict(source_ja=original[index]['text'],
            translation_en=text,source=dict(file_sha256=digest(data)))
    generated={}
    if args.include_model_drafts:
        from translate_english_local import mask,validate
        for draft_path in (base/'model-drafts').glob('gop_*.json'):
            draft=json.loads(draft_path.read_text(encoding='utf8'))
            source=(ROOT/draft['source_file']).read_bytes()
            if digest(source)!=draft['source_sha256']:raise ValueError('Stale local-model source')
            for key,row in draft['entries'].items():
                # Revalidate the final restored text, independently of the worker.
                candidate,saved=mask(row['translation'])
                checked,reason=validate(row['source'],candidate,saved)
                if reason:raise ValueError('Invalid local draft '+key+': '+reason)
                generated[key]=dict(row,translation=checked,model=row.get('model',draft['model']))
    files = []
    for ko_path in sorted((ROOT / 'localization/ko-KR/structured').glob('*.json')):
        # The Korean catalog supplies the proven scope, never English target text.
        scope = json.loads(ko_path.read_text(encoding='utf-8'))
        source = ROOT / scope['source_file']
        data = source.read_bytes()
        if digest(data) != scope['source_sha256']:
            raise ValueError(f'Source hash mismatch: {source}')
        original = {r['index']: r for r in parse(data)}
        target = base / 'structured' / ko_path.name
        previous = json.loads(target.read_text(encoding='utf-8')) if target.exists() else {'entries': []}
        old = {r['id']: r for r in previous['entries']}
        entries = []
        for scoped in scope['entries']:
            row = original[scoped['index']]
            if row['text'] != scoped['source'] or row['metadata']:
                raise ValueError(f'Source identity mismatch: {scoped["id"]}')
            prior = old.get(scoped['id'], {})
            if prior and prior['source'] != row['text']:
                raise ValueError(f'Stale English translation: {scoped["id"]}')
            entry = seeds.get(scoped['id'], {})
            if entry and (entry['source_ja'] != row['text'] or entry['source']['file_sha256'] != digest(data)):
                raise ValueError(f'Stale English seed: {scoped["id"]}')
            model=generated.get(scoped['id'],{})
            if model and (model['source']!=row['text'] or model['index']!=row['index']):raise ValueError('Model draft identity changed')
            if prior.get('status')=='local_model_draft_needs_review' and entry.get('translation_en'):
                text=entry['translation_en']
            else:text = prior.get('translation') or entry.get('translation_en', '') or model.get('translation','')
            held = ''
            if text and (TOKEN.findall(text) != TOKEN.findall(row['text']) or '\x00' in text):
                if prior.get('translation'):
                    raise ValueError(f'Invalid English control tokens: {scoped["id"]}')
                held, text = text, ''
            entries.append(dict(id=scoped['id'], index=row['index'], offset=row['offset'],
                                  source=row['text'], translation=text.replace('·','-').replace('—','-').replace('–','-').replace('。','.').replace('、',','),
                                status='english_draft' if text else 'untranslated'))
            if held:
                entries[-1].update(held_translation=held, hold_reason='Control-token identity requires review')
            if text and text==prior.get('translation') and prior.get('status')=='local_model_draft_needs_review':
                entries[-1].update({k:prior[k] for k in ('status','model','human_translation_reviewed') if k in prior})
            if text and model and text==model['translation']:
                entries[-1].update(status='local_model_draft_needs_review',model=model.get('model','recorded in model-drafts'),human_translation_reviewed=False)
        document = dict(locale='en-US', source_file=source.relative_to(ROOT).as_posix(),
                        source_sha256=digest(data), entries=entries, runtime_verified=False)
        # Build and roundtrip before publishing this catalog's artifacts.
        output, changed = patch(data, entries)
        patched = base / 'build/gop' / source.name
        patched.parent.mkdir(parents=True, exist_ok=True)
        patched.write_bytes(output)
        write(target, document)
        files.append(dict(file=source.name, entries=len(entries), translated=len(changed),
                          remaining=len(entries)-len(changed), sha256=digest(output)))
    report = dict(locale='en-US', status='INCOMPLETE_NOT_INSTALLABLE',
                  files=files, total=sum(r['entries'] for r in files),
                  translated=sum(r['translated'] for r in files),
                  remaining=sum(r['remaining'] for r in files),
                  runtime_verified=False, installed=False,
                  other_required_components=['ELF UI', 'image UI', 'movie subtitles', 'battle voice captions',
                                             'independent VFS and archive assembly', 'in-game layout review'])
    if not report['remaining']:report['status']='FULL_TEXT_DRAFT_REVIEW_PENDING'
    write(base / 'build/gop-manifest.json', report)
    print(json.dumps({k: v for k, v in report.items() if k != 'files'}, ensure_ascii=False))


if __name__ == '__main__':
    main()
