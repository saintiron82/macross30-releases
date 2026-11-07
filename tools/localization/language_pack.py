"""Author, build, inspect and import data-only local language packs."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
import zipfile
import unicodedata
import tempfile

ROOT=Path(__file__).resolve().parents[2]
SCOPE=ROOT/'localization/ja-en-ru-review'
sys.path.insert(0,str(ROOT/'tools/play'))
from pack_registry import valid_locale,digest,validate_pack,import_pack,discover_packs


def read(p):return json.loads(Path(p).read_text(encoding='utf-8-sig'))
def write(p,d):Path(p).write_text(json.dumps(d,ensure_ascii=False,indent=2)+'\n',encoding='utf8')


def initialize(locale,label,output):
    if not valid_locale(locale):raise ValueError('Invalid locale identifier')
    out=Path(output);out.mkdir(parents=True,exist_ok=False)
    gop=read(SCOPE/'gop-scope.json');elf=read(SCOPE/'elf-scope.json')
    entries={r['id']:dict(source=r['source_ja'],source_sha256=r['source_sha256'],text=None) for r in gop['entries']}
    entries.update({r['id']:dict(source=r['source_ja'],source_sha256=r['source_sha256'],text=None) for r in elf['entries']})
    write(out/'project.json',dict(schema_version=1,locale=locale,label=label,base_locale='en-US',image_locale='en-US',
        source_scope_sha256=dict(gop=digest(SCOPE/'gop-scope.json'),elf=digest(SCOPE/'elf-scope.json')),
        entries=entries,native_ui={},description='User-authored local text pack; English images.'))
    (out/'README.txt').write_text('Edit project.json entries[*].text; null means explicit English fallback.\nDo not alter source or source_sha256. Keep placeholders and control tokens.\nBuild: python tools/localization/language_pack.py build --project THIS_FOLDER/project.json --output NEW_FOLDER --zip\nImport ZIP or generated pack.json using the local options panel. Partial packs need explicit opt-in.\n',encoding='utf8')
    return out/'project.json'


def validate_authored(doc, gop, elf, native_scope):
    """Validate authored data before creating any output or touching baselines."""
    from gop_strings import TOKEN
    originals={r['id']:r for r in gop['entries']+elf['entries']}
    entries=doc.get('entries',{})
    ui=doc.get('native_ui',{})
    if not isinstance(entries,dict) or not isinstance(ui,dict):raise ValueError('Entries and native_ui must be objects')
    if not isinstance(doc.get('label'),str) or not doc['label'].strip() or len(doc['label'])>100 or any(ord(c)<32 for c in doc['label']):raise ValueError('Invalid language label')
    values={}
    for key,item in entries.items():
        source=originals.get(key)
        if source is None:raise ValueError('Unknown text key: '+key)
        if not isinstance(item,dict) or item.get('source_sha256')!=source['source_sha256'] or item.get('source')!=source['source_ja']:raise ValueError('Original source altered: '+key)
        text=item.get('text')
        if text is None or text=='':continue
        if not isinstance(text,str) or not text.strip() or '\0' in text:raise ValueError('Invalid text: '+key)
        text=unicodedata.normalize('NFC',text)
        if TOKEN.findall(text)!=TOKEN.findall(source['source_ja']):raise ValueError('Control tokens changed: '+key)
        if any(0xD800<=ord(c)<=0xDFFF for c in text):raise ValueError('Invalid Unicode text: '+key)
        values[key]=text
    known_ui={row['key']:row['source'] for row in native_scope}
    for key,text in ui.items():
        if key not in known_ui:raise ValueError('Unknown native UI key: '+key)
        if not isinstance(text,str) or not text.strip():raise ValueError('Invalid native UI entry: '+key)
        if any((ord(c)<32 and c not in '\n\t') or 0x7F<=ord(c)<0xA0 or 0xD800<=ord(c)<=0xDFFF for c in text):raise ValueError('Invalid native UI control character: '+key)
        if TOKEN.findall(text)!=TOKEN.findall(known_ui[key]):raise ValueError('Native UI control tokens changed: '+key)
        encoded=text.replace('\\','\\\\').replace('\n','\\n').replace('\t','\\t').encode('utf8')
        if len(key.encode())+1+len(encoded)+1>=16384:raise ValueError('Native UI line exceeds loader limit: '+key)
    if not values and not ui:raise ValueError('No translated entries')
    return values,ui


def validate_font(values):
    """The game renders these resources with ABF; OS-native UI has its own font."""
    if not values:return
    from english_font import original_font
    glyphs=original_font()[2]
    for key,text in values.items():
        missing=sorted({ord(c) for c in text if c not in '\n\r\t' and ord(c) not in glyphs})
        if missing:raise ValueError('Unsupported game font glyphs in '+key+': '+', '.join(f'U+{cp:04X}' for cp in missing))


def read_caption_payloads(directory,locale):
    import re
    directory=Path(directory);report=read(directory/'file-coverage.json')
    if report.get('locale')!=locale:raise ValueError('Caption locale mismatch')
    scope=read(SCOPE/'caption-scope.json')
    if report.get('source_scope_sha256')!=digest(SCOPE/'caption-scope.json'):raise ValueError('Caption source scope changed')
    movies={m['movie']:m for m in scope['movies']};payloads={}
    registry=(directory/'tracks.tsv').read_text(encoding='utf8').splitlines()
    if not registry or registry[0]!='MACROSS_SUBTITLES_V1':raise ValueError('Invalid movie registry header')
    seen=set()
    for line in registry[1:]:
        fields=line.split('\t')
        if len(fields)!=7 or fields[0] not in movies or fields[0] in seen:raise ValueError('Invalid/duplicate movie registry entry')
        movie=movies[fields[0]];name=fields[1]
        if name!=Path(movie['movie']).stem+'.'+locale.split('-')[0]+'.srt':raise ValueError('Unexpected caption path')
        raw=(directory/name).read_bytes()
        if digest(directory/name)!=fields[5] or fields[4]!=movie['movie_sha256'] or int(fields[2])!=movie['origin90k']:raise ValueError('Movie subtitle proof changed')
        if b'\0' in raw:raise ValueError('NUL in subtitle')
        raw.decode('utf8');payloads[name]=raw;seen.add(fields[0])
    voice_name='battle.'+locale.split('-')[0]+'.tsv';voice=(directory/voice_name).read_text(encoding='utf8').splitlines()
    if not voice or voice[0]!='MACROSS_BATTLE_CAPTIONS_V3\t'+locale:raise ValueError('Voice caption locale/header mismatch')
    known={r['stem']:r for r in scope['voice']};voice_seen=set();previous=''
    for line in voice[1:]:
        fields=line.split('\t')
        if len(fields)!=3 or fields[0] not in known or fields[0]<=previous:raise ValueError('Voice key invalid/duplicate/unsorted')
        key,duration,text=fields
        if duration!=known[key]['duration'] or not text or '\0' in text:raise ValueError('Voice duration or text changed')
        previous=key;voice_seen.add(key)
    if report['voice_translated']!=len(voice_seen):raise ValueError('Voice coverage report mismatch')
    completed={r['movie'] for r in report['movies'] if r['status']=='translated_draft'}
    if seen!=completed:raise ValueError('Movie file coverage report mismatch')
    payloads['tracks.tsv']=(directory/'tracks.tsv').read_bytes();payloads[voice_name]=(directory/voice_name).read_bytes()
    payloads['file-coverage.json']=(directory/'file-coverage.json').read_bytes()
    return payloads,dict(movie_cues=report['movie_translated'],movie_total=sum(len(m['cues']) for m in scope['movies']),voice_stems=len(voice_seen),voice_total=len(scope['voice']))


def build(project,output,archive=False,font_path=None,captions_path=None,require_complete=False,proportional_cyrillic=False,compact_russian=False):
    from gop_strings import TOKEN
    from build_russian_pack import rebuild_gop,cyrillic_font
    from english_font import original_font
    from stage_gop_primary_mirrors import pidx,read_payload
    from build_russian_pack import packfile
    from build_runtime_elf_text import build as build_elf
    doc=read(project)
    if doc.get('schema_version')!=1 or not valid_locale(doc.get('locale')):raise ValueError('Invalid authoring project')
    if doc.get('base_locale')!='en-US' or doc.get('image_locale')!='en-US':raise ValueError('Authoring compiler v1 uses frozen English resources')
    for kind in ('gop','elf'):
        if doc['source_scope_sha256'][kind]!=digest(SCOPE/(kind+'-scope.json')):raise ValueError('Source scope changed; regenerate/reconcile project')
    gop=read(SCOPE/'gop-scope.json');elf=read(SCOPE/'elf-scope.json')
    old=read(ROOT/'localization/ru-RU/scope.json')
    values,ui=validate_authored(doc,gop,elf,old['native_ui'])
    caption_payloads={};caption_counts=dict(movie_cues=0,movie_total=sum(len(m['cues']) for m in old['movies']),voice_stems=0,voice_total=len(old['voice']))
    if captions_path:caption_payloads,caption_counts=read_caption_payloads(captions_path,doc['locale'])
    display_values=dict(values)
    for name,payload in caption_payloads.items():
        if name.endswith(('.srt','.tsv')):display_values['caption:'+name]=payload.decode('utf8')
    font_payloads={};font_report=None
    if font_path and values:
        from pack_font import build as extend_font
        from stage_gop_primary_mirrors import fsts_entries
        with tempfile.TemporaryDirectory(prefix='macross-pack-font-') as work:
            work=Path(work);inputs=work/'source';inputs.mkdir()
            source=ROOT/'native/vfs/PS3_GAME/USRDIR/DATA/PACK'
            _,_,archives=pidx(source/'PACK.IDX')
            resident=next(r for r in archives if r['name']=='resident')
            entries=fsts_entries(read_payload(packfile(source,resident['pack']),resident)[0])
            for name in ('font_j24x24.abf','font_j24x24_0.dds'):
                (inputs/name).write_bytes(next(e['payload'] for e in entries if e['name']=='data/font/'+name))
            font_report=extend_font(inputs/'font_j24x24.abf',inputs/'font_j24x24_0.dds',font_path,''.join(display_values.values()),work/'extended',proportional_cyrillic_metrics=proportional_cyrillic,compact_russian=compact_russian)
            font_payloads={name:(work/'extended'/name).read_bytes() for name in ('font_j24x24.abf','font_j24x24_0.dds')}
    else:validate_font(display_values)
    out=Path(output);out.mkdir(parents=True,exist_ok=False)
    ready=read(ROOT/'localization/en-US/pack-ready.json');baseline=ROOT/ready['elf'];resources={}
    pack=ROOT/ready['vfs']/'PS3_GAME/USRDIR/DATA/PACK'
    _,rows,_=pidx(pack/'PACK.IDX')
    by_catalog={}
    for r in gop['entries']:
        if r['id'] in values:by_catalog.setdefault(r['catalog'],{})[r['index']]=values[r['id']]
    if by_catalog or font_payloads:
        overlay=out/'gop';overlay.mkdir()
        for catalog,targets in by_catalog.items():
            name=catalog+'.gop'
            row=next(r for r in rows if r['name']==name)
            raw=read_payload(packfile(pack,row['pack']),row)[0]
            (overlay/name).write_bytes(rebuild_gop(raw,targets))
        # Existing glyphs only. Metrics are data and travel with the pack.
        if font_payloads:
            for name,payload in font_payloads.items():(overlay/name).write_bytes(payload)
            write(out/'font-provenance.json',font_report)
        else:
            font=original_font()[0]
            if any('\u0400'<=c<='\u052f' for t in values.values() for c in t):font=cyrillic_font(font)[0]
            (overlay/'font_j24x24.abf').write_bytes(font)
        resources['gop_overlay']='gop'
    elf_texts={str(r['offset']):values[r['id']] for r in elf['entries'] if r['id'] in values}
    elf_report={'records':0,'total':len(elf['entries']),'fallback_count':len(elf['entries'])}
    if elf_texts:
        write(out/'elf-authored.json',elf_texts)
        bindings=SCOPE/'elf-display-bindings.json'
        elf_report=build_elf(SCOPE/'elf-scope.json',baseline,out/'elf-authored.json',out/'elf-text.pack',bindings_path=bindings if bindings.exists() else None)
        resources['elf_overlay']='elf-text.pack'
    if ui:
        from re import fullmatch
        lines=['MACROSS_UI_STRINGS_V1\t'+doc['locale']]
        for key,text in sorted(ui.items()):
            if not fullmatch(r'[A-Za-z0-9_.-]{1,95}',key) or not isinstance(text,str) or not text or '\0' in text:raise ValueError('Invalid native UI entry')
            lines.append(key+'\t'+text.replace('\\','\\\\').replace('\n','\\n').replace('\t','\\t'))
        (out/'native-ui.tsv').write_text('\n'.join(lines)+'\n',encoding='utf8');resources['native_ui']='native-ui.tsv'
    if caption_payloads:
        subs=out/'subtitles';subs.mkdir()
        for name,payload in caption_payloads.items():(subs/name).write_bytes(payload)
        resources.update(subtitles='subtitles',battle_catalog='subtitles/battle.'+doc['locale'].split('-')[0]+'.tsv')
    if not resources:raise ValueError('No buildable translated resources')
    total=len(gop['entries'])+len(elf['entries'])+caption_counts['movie_total']+caption_counts['voice_total']+len(old['native_ui'])
    translated=sum(len(x) for x in by_catalog.values())+elf_report['records']+len(ui)+caption_counts['movie_cues']+caption_counts['voice_stems']
    report=dict(total=total,translated=translated,fallback=total-translated)
    complete=report['fallback']==0 and {'gop_overlay','elf_overlay','native_ui','subtitles','battle_catalog'} <= set(resources)
    if require_complete and not complete:raise ValueError('Complete pack required; missing translated/applied resources: '+str(report))
    write(out/'coverage.json',dict(coverage=report,gop_translated=sum(len(x) for x in by_catalog.values()),elf=elf_report,
        movie_cues_translated=caption_counts['movie_cues'],voice_stems_translated=caption_counts['voice_stems'],native_ui_translated=len(ui),quality='full-scope translation draft; light audit recorded' if complete else 'test preview; incomplete text coverage',
        fallback_languages=[] if complete else ['en-US'],limitations=['English UI artwork is intentionally inherited. Original song titles and credits are retained.','ASR source uncertainty is recorded in subtitles/file-coverage.json; no human/native/audio review is claimed.'] if complete else ['Separate movie/voice captions inherit English until explicitly supplied.','ELF strings without verified pointers may remain English.']))
    manifest=dict(schema_version=1,locale=doc['locale'],label=doc['label'],description=doc.get('description',''),status='complete' if complete else 'partial_test',base_locale='en-US',image_locale='en-US',coverage=report,resources=resources,
        source_guards=[dict(path='localization/en-US/pack-ready.json',sha256=digest(ROOT/'localization/en-US/pack-ready.json'))],
        proofs=[dict(path=p.relative_to(out).as_posix(),sha256=digest(p)) for p in sorted(out.rglob('*')) if p.is_file()])
    write(out/'pack.json',manifest);validate_pack(ROOT,out)
    if archive:
        target=out.with_suffix('.zip')
        with zipfile.ZipFile(target,'x',zipfile.ZIP_DEFLATED) as z:
            for p in sorted(out.rglob('*')):
                if p.is_file():z.write(p,p.relative_to(out).as_posix())
    return manifest


def main():
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='action',required=True)
    q=sub.add_parser('init');q.add_argument('--locale',required=True);q.add_argument('--label',required=True);q.add_argument('--output',required=True)
    q=sub.add_parser('build');q.add_argument('--project',required=True);q.add_argument('--output',required=True);q.add_argument('--zip',action='store_true');q.add_argument('--font',help='Local TTF/OTF for missing glyphs; raster data only');q.add_argument('--captions',help='Validated locale movie/voice caption directory');q.add_argument('--compact-russian',action='store_true',help='80 percent Russian and Latin text glyphs');q.add_argument('--proportional-cyrillic',action='store_true',help='Use proportional Russian glyph spacing');q.add_argument('--require-complete',action='store_true',help='Reject any untranslated or unapplied resource')
    q=sub.add_parser('audit');q.add_argument('source')
    q=sub.add_parser('import');q.add_argument('source')
    sub.add_parser('list');a=p.parse_args()
    if a.action=='init':print(initialize(a.locale,a.label,a.output))
    elif a.action=='build':
        m=build(a.project,a.output,a.zip,a.font,a.captions,a.require_complete,a.proportional_cyrillic,a.compact_russian)
        print(json.dumps(dict(locale=m['locale'],status=m['status'],coverage=m['coverage'],output=a.output),ensure_ascii=False))
    elif a.action=='audit':print(json.dumps(validate_pack(ROOT,Path(a.source).parent if Path(a.source).is_file() else Path(a.source))[0],ensure_ascii=False))
    elif a.action=='import':print(json.dumps(import_pack(ROOT,a.source),ensure_ascii=False))
    else:print(json.dumps(discover_packs(ROOT),ensure_ascii=False,indent=2))
if __name__=='__main__':main()
