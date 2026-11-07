"""Admit a complete Russian text draft and export its locale overlay."""
import json,hashlib,zipfile
from pathlib import Path
from russian_pack import ROOT,BASE,read,write,sha
from build_russian_pack import OUTPUT,STATE,SUBS
def digest(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as f:
        for b in iter(lambda:f.read(8*1024*1024),b''):h.update(b)
    return h.hexdigest()
def main():
    verification=read(BASE/'build/verification.json');assert verification['status']=='PASS'
    candidate=ROOT/'native/subtitle-candidate/russian-text-v1';build=read(candidate/'build-result.json')
    assert build['status']=='compiled_and_linked_candidate_only'
    assert digest(candidate/'Macross30Recompiled.exe')==build['products']['exe']['sha256']
    proof_files=[OUTPUT/'EBOOT.russian.elf',SUBS/'battle.ru.tsv',SUBS/'tracks.tsv',BASE/'build/verification.json',candidate/'Macross30Recompiled.exe',candidate/'baseline-audit.json']
    proof_files+=list(SUBS.glob('*.srt'))
    packs=OUTPUT/'PS3_GAME/USRDIR/DATA/PACK';proof_files+=list(packs.iterdir())
    ready=dict(locale='ru-RU',image_locale='en-US',status='full_draft',untranslated=0,
               elf=(OUTPUT/'EBOOT.russian.elf').relative_to(ROOT).as_posix(),vfs=OUTPUT.relative_to(ROOT).as_posix(),
               state=STATE.relative_to(ROOT).as_posix(),subtitles=SUBS.relative_to(ROOT).as_posix(),battle_catalog=(SUBS/'battle.ru.tsv').relative_to(ROOT).as_posix(),
               candidate_dir='russian-text-v1',native_baseline_version='0.3.4',song_policy='original',
               human_translation_review_complete=False,game_render_verified=False,
               proofs=[dict(path=p.relative_to(ROOT).as_posix(),sha256=digest(p)) for p in proof_files if p.is_file()])
    # Admit the actual resource files only after all build/readback checks pass.
    write(BASE/'pack-ready.json',ready)
    import sys
    sys.path.insert(0,str(ROOT/'tools/play'))
    from language_packs import resolve_language
    from native_binary_launch import select_native_binary
    resolved=resolve_language(ROOT,'ru-RU');select_native_binary(ROOT,resolved['candidate_dir'])
    target=ROOT/'final/exports/Macross30-ru-RU-text-v1-draft.zip';assert not target.exists()
    files={}
    def add(member,path):
        path=Path(path);assert member not in files;files[member]=dict(path=path,sha256=digest(path),bytes=path.stat().st_size)
    add('elf/EBOOT.russian.elf',OUTPUT/'EBOOT.russian.elf')
    add('fonts/font_j24x24.abf',BASE/'build/font_j24x24.abf')
    add('fonts/font-metrics.json',BASE/'build/font-metrics.json')
    for p in (BASE/'build/gop').glob('*.gop'):add('gop/'+p.name,p)
    for p in SUBS.iterdir():add('subtitles/'+p.name,p)
    for name in ('Macross30Recompiled.exe','SDL2.dll','baseline-audit.json','plan.json','build-result.json'):add('native/'+name,candidate/name)
    for name in ('scope.json','translations.json'):add('catalogs/'+name,BASE/name)
    add('verification.json',BASE/'build/verification.json')
    manifest=dict(locale='ru-RU',image_locale='en-US',status='machine_draft_review_pending',native_baseline_version='0.3.4',
                  scope=verification,images='Use the existing English image pack unchanged; no image replacements in this text overlay.',
                  song_policy='Original titles/credits retained',files=[dict(member=k,sha256=v['sha256'],bytes=v['bytes']) for k,v in files.items()])
    with zipfile.ZipFile(target,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for name,row in files.items():z.write(row['path'],name)
        z.writestr('pack-manifest.json',json.dumps(manifest,ensure_ascii=False,indent=2))
        z.writestr('README.txt','Macross 30 Russian text draft, original 0.3.4 native baseline.\nImages: existing English pack, unchanged. Song titles remain original.\nLocal launch: python tools/play/start_russian_preview.py\nRequires original game data and this Macross Windows workspace. GOP text and ELF data are translated; full game and human linguistic review remain pending.\nThe native/assets/aircraft-hd.pack from the existing installation is optional and unchanged.\n')
    with zipfile.ZipFile(target) as z:assert z.testzip() is None
    result=dict(status='full_draft_review_pending',path=str(target),sha256=digest(target),bytes=target.stat().st_size,members=len(files)+2,locale='ru-RU',image_locale='en-US')
    write(BASE/'resource-bundle.json',result);print(json.dumps(result),flush=True)
if __name__=='__main__':main()
