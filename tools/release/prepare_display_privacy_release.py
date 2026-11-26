"""Build a private-path-free display release from explicitly frozen packages."""
import argparse
import ast
import copy
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import zipfile

ROOT=Path(__file__).resolve().parents[2]
sys.dont_write_bytecode=True
sys.path[:0]=[str(ROOT/'tools/release'),str(ROOT/'tools/dist'),str(ROOT/'tools/local_build')]
from privacy import sanitize_pe_pdb,sanitize_shader_manifest,relative_source_paths,check_owned_bytes,digest
from pipeline import META,prepare,verify,write,sha
from make_dist import make_delta,apply_delta_sha
from elf_patch import apply

def encoded(doc):return (json.dumps(doc,ensure_ascii=False,indent=2)+'\n').encode()
def unpack(path):
    with zipfile.ZipFile(path) as z:return {n:z.read(n) for n in z.namelist() if not n.endswith('/')}
def pack(path,files):
    path.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(path,'x',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for name,data in sorted(files.items()):
            info=zipfile.ZipInfo(name,date_time=(2026,1,1,0,0,0));info.create_system=3;info.external_attr=0o100644<<16
            z.writestr(info,data,compress_type=zipfile.ZIP_DEFLATED,compresslevel=6)
def identity(path):return dict(file=path.name,bytes=path.stat().st_size,sha256=sha(path))

def main(a):
    output=a.output.resolve();assert output.is_relative_to(ROOT/'build/releases')
    assert not output.exists(),'Use a fresh candidate work folder'
    output.mkdir(parents=True)
    tests=ROOT/'build/tests'/('display-privacy-'+a.version)
    tests.mkdir(parents=True,exist_ok=True);tmp=tests/'tmp';tmp.mkdir(exist_ok=True)
    tempfile.tempdir=str(tmp);os.environ.update(TEMP=str(tmp),TMP=str(tmp),TMPDIR=str(tmp))
    base=a.previous.resolve();oldplan=json.loads((base/'candidate-final/release-plan.json').read_text(encoding='utf-8'))
    boot=json.loads((base/'baseline/BOOT-INSTALL.json').read_text(encoding='utf-8'))
    baseline=output/'baseline';baseline.mkdir()
    for name in META:shutil.copyfile(base/'baseline'/name,baseline/name)
    inputs=output/'inputs';inputs.mkdir()
    oldprogram=base/'packages'/oldplan['components']['game']['artifact']['file']
    assert sha(oldprogram)==oldplan['components']['game']['artifact']['sha256']
    files=unpack(oldprogram);before=dict(files)
    exe,peproof=sanitize_pe_pdb(files['program/bin/Macross30Recompiled.exe'])
    assert peproof['pdb_paths_generalized']>0
    files['program/bin/Macross30Recompiled.exe']=exe
    print('PE metadata sanitized; executable sections unchanged',flush=True)
    shader,shaderproof=sanitize_shader_manifest(files['program/bin/assets/shader-seed-manifest-v3.json'])
    files['program/bin/assets/shader-seed-manifest-v3.json']=shader
    guard=json.loads(files['program/bin/assets/locale-base.json'])
    guard['runtime_exe_sha256']=digest(exe);guard['runtime_release_version']=a.version
    guardbytes=encoded(guard);files['program/bin/assets/locale-base.json']=guardbytes
    program=json.loads(files.pop('program/program-manifest.json'))
    program['version']=a.version;program['shader_prep']['manifest_sha256']=shaderproof['manifest_sha256']
    program['files']=[dict(path=n[len('program/'):],bytes=len(data),sha256=digest(data)) for n,data in sorted(files.items())]
    files['program/program-manifest.json']=encoded(program)
    changed=sorted(n for n in files if files[n]!=before[n])
    assert changed==['program/bin/Macross30Recompiled.exe','program/bin/assets/locale-base.json',
                    'program/bin/assets/shader-seed-manifest-v3.json','program/program-manifest.json']
    program_path=inputs/f'Macross30-program-{a.version}.zip';pack(program_path,files)
    nextboot=copy.deepcopy(boot);nextboot['version']=a.version
    nextboot['files']['program']={**identity(program_path),'exe_sha256':digest(exe)}
    nextboot['features']['locale_packs']['runtime_release_version']=a.version
    write(output/'PE-METADATA-PROOF.json',peproof)
    write(output/'SHADER-METADATA-PROOF.json',shaderproof)

    # Freeze the new Korean ELF and prove every changed byte belongs to an authored slot.
    display=json.loads(a.display_proof.read_text(encoding='utf-8'))
    authored=json.loads((a.display_proof.parent/'korean-authored.json').read_text(encoding='utf-8'))
    assert display['status']=='PASS' and display['korean_strings']==154 and not display['font_missing']
    original=a.original_elf.read_bytes();korean=a.korean_elf.read_bytes()
    assert digest(original)==boot['original_elf_sha256'] and digest(korean)==display['output_sha256']
    ko_old=a.downloads/boot['language_packs']['ko-KR']['file'];assert sha(ko_old)==boot['language_packs']['ko-KR']['sha256']
    ko=unpack(ko_old);ko_before=dict(ko);recipe=json.loads(ko['payload/recipe.json']);kr=recipe['languages']['ko-KR']
    elfmember='payload/'+kr['elf_patch'];oldelf=apply(original,ko[elfmember])
    assert digest(oldelf)==display['source_sha256']
    allowed=set()
    for row in authored['entries']:
        off=int(row['offset'],0);cap=row['capacity'];allowed.update(range(off,off+cap))
        assert korean[off:off+cap].split(b'\0',1)[0].decode()==row['translation']
    assert len(oldelf)==len(korean) and all(i in allowed for i,(x,y) in enumerate(zip(oldelf,korean)) if x!=y)
    delta,stats=make_delta(a.original_elf,a.korean_elf,digest(original),digest(korean),2048)
    assert apply(original,delta)==korean
    assert apply_delta_sha(delta,a.original_elf)==(len(korean),digest(korean))
    recipe['version']=a.version;kr['elf_sha256']=digest(korean)
    ko[elfmember]=delta;ko['payload/recipe.json']=encoded(recipe)
    ko_changed=sorted(n for n in ko if ko[n]!=ko_before[n])
    assert ko_changed==[elfmember,'payload/recipe.json']
    ko_path=inputs/f'Macross30-language-ko-KR-{a.version}.zip';pack(ko_path,ko)
    nextboot['language_packs']['ko-KR']={**boot['language_packs']['ko-KR'],**identity(ko_path),'elf_sha256':digest(korean)}
    write(inputs/'BOOT-INSTALL.json',nextboot)
    print('Korean 154 strings repacked; delta readback passed',flush=True)

    # Only the source guard changes in each text pack. Russian starts from reviewed v7.
    locales=output/'locales';locales.mkdir();updates={};text_proofs={}
    for locale,row in oldplan['text_packs'].items():
        oldrow=row['artifact'];source=Path(row['path']);assert sha(source)==oldrow['sha256']
        text=unpack(source);prior=dict(text);doc=json.loads(text['pack.json'])
        assert doc['source_guards']==[dict(path='bin/assets/locale-base.json',sha256=digest(before['program/bin/assets/locale-base.json']))]
        doc['source_guards']=[dict(path='bin/assets/locale-base.json',sha256=digest(guardbytes))]
        text['pack.json']=encoded(doc)
        assert [n for n in text if text[n]!=prior[n]]==['pack.json']
        path=locales/f'Macross30-text-{locale}-{a.version}.zip';pack(path,text)
        updates[locale]={**oldrow,**identity(path),'manifest_sha256':digest(text['pack.json'])}
        text_proofs[locale]=dict(changed_members=['pack.json'],content_bytes_identical=True,revision=oldrow['text_revision'])
        if locale in ('ru-RU','es-ES'):
            payload=text['elf-text.pack'];count=struct.unpack_from('<I',payload,40)[0];pos=48;records={}
            for _ in range(count):
                ea,cap,length,refs=struct.unpack_from('<IIII',payload,pos);pos+=16
                expected=payload[pos:pos+cap];pos+=cap
                translation=payload[pos:pos+length].decode();pos+=length+4*refs
                records[ea]=(expected,translation)
            for item in display['locales'][locale]['entries']:
                off=int(item['offset'],0);assert records[off+0x10000][1]==item['text']
    english=a.downloads/boot['language_packs']['en-US']['file']
    assert sha(english)==boot['language_packs']['en-US']['sha256']
    ef=unpack(english);er=json.loads(ef['payload/recipe.json'])['languages']['en-US']
    en=apply(original,ef['payload/'+er['elf_patch']]);assert digest(en)==guard['english_elf_sha256']
    for item in display['locales']['en-US']['entries']:
        off=int(item['offset'],0);assert en[off:en.index(0,off)].decode()==item['text']
    print('English/Russian/Spanish 154-string package readbacks passed',flush=True)

    # Public source snapshot: retain history separately; remove local paths from the next tree.
    public=output/'public-source';public.mkdir();original_public={}
    with zipfile.ZipFile(a.public_source) as z:
        for info in z.infolist():
            if info.is_dir():continue
            name=info.filename.split('/',1)[1];data=z.read(info);original_public[name]=data
            target=public/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
    overlays=['tools/release/privacy.py','tools/release/prepare_display_privacy_release.py','tools/shader_cache/shader_seed.py']
    for name in overlays:
        target=public/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes((ROOT/name).read_bytes())
    reports={'docs/four-locale-display-fix-20261115.md':a.display_report,
             'release-baselines/'+a.version+'/korean-authored.json':a.display_proof.parent/'korean-authored.json'}
    for name,source in reports.items():
        target=public/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(source.read_bytes())
    for name in ['tools/local_build/setup_tools.ps1','tools/tests/test_setup_python_probe.py',
                 'tools/localization/complete_four_locale_display.py','tools/localization/korean_remaining_text.py']:
        target=public/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes((ROOT/name).read_bytes())
    cmake=public/'native/macross30/CMakeLists.txt'
    cmake_text=cmake.read_text(encoding='utf-8-sig')
    cmake_text=cmake_text.replace('"/PDB:${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.pdb")',
                                '"/PDB:${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.pdb" "/PDBALTPATH:${PROJECT_NAME}.pdb")')
    marker='if(MSVC AND MACROSS_LINK_MAP)'
    if marker in cmake_text:
        cmake_text=cmake_text.replace(marker,'if(MSVC)\n    target_link_options(${PROJECT_NAME} PRIVATE "/PDBALTPATH:${PROJECT_NAME}.pdb")\nendif()\n'+marker,1)
    cmake.write_text(cmake_text,encoding='utf-8')
    readme_replacements={}
    for name in ('README.md','README.en.md'):
        original_text=(public/name).read_text(encoding='utf-8-sig')
        headline=next(line for line in original_text.splitlines() if line.startswith('**') and '0.3.11' in line)
        new_headline=('**0.3.12: 한국어 누락 문구 154개, Python 설치 준비 수정, 러시아어 v7 및 배포 경로 정보 정리. 런처 0.3.9 · 영어 기반팩 0.3.8.**' if name=='README.md' else
                      '**0.3.12: 154 Korean display strings, Python setup fix, Russian v7 and release path cleanup. Launcher: 0.3.9; English base pack: 0.3.8.**')
        edits=[dict(old=headline,new=new_headline)]
        for old,new in [('https://github.com/saintiron82/macross30-releases/releases/tag/v0.3.11',f'https://github.com/saintiron82/macross30-releases/releases/tag/v{a.version}'),
                        ('러시아어 v6-r2·스페인어 v3','러시아어 v7·스페인어 v3'),
                        ('**Russian v6-r2 / Spanish v3:**','**Russian v7 / Spanish v3:**'),
                        ('게임은 0.3.10, 런처는 0.3.9, 한글·영어 기반팩은 0.3.8입니다. 러시아어 텍스트팩은 0.3.11(v6-r2), 영어·스페인어 텍스트팩은 0.3.10입니다.',
                         '프로그램·한국어 팩·추가 텍스트팩은 0.3.12이며, 런처 0.3.9와 영어 기반팩 0.3.8을 사용합니다.'),
                        ('Game is 0.3.10; launcher remains 0.3.9; Korean/English base packs remain 0.3.8. The Russian text pack is 0.3.11 (v6-r2); English/Spanish text packs remain 0.3.10.',
                         'Program, Korean pack and additional text packs are 0.3.12; launcher remains 0.3.9 and English base pack remains 0.3.8.')]:
            if original_text.count(old)==1:edits.append(dict(old=old,new=new))
        text=original_text.replace('Game **0.3.10**',f'Game **{a.version}**')
        for edit in edits:
            assert text.count(edit['old'])==1;text=text.replace(edit['old'],edit['new'],1)
        (public/name).write_text(text,encoding='utf-8');readme_replacements[name]=edits
    for target in public.rglob('*'):
        if not target.is_file():continue
        data=target.read_bytes()
        try:text=re.sub(r'\r+\n','\n',data.decode('utf-8-sig')).replace('\r','\n')
        except UnicodeDecodeError:continue
        cleaned=relative_source_paths(text)
        if cleaned!=text or b'\r\r\n' in data:target.write_bytes(cleaned.encode())
        if target.suffix=='.py':ast.parse(cleaned,filename=target.relative_to(public).as_posix())
        if target.suffix=='.json':json.loads(cleaned)
        check_owned_bytes(target.read_bytes())
    print('Public source local paths generalized; Python/JSON syntax passed',flush=True)
    notes=f'''Macross 30 {a.version} — Korean display fixes and release privacy cleanup

한국어
- 기체·추가 장비·인물 소개 등의 누락 문구 154개를 한국어 팩에 반영했습니다. 영어·러시아어·스페인어의 같은 154개 문구는 실제 배포 입력에서 확인했습니다.
- 설치 준비 단계의 구버전/사용 불가 Python 검사 오류 수정과 러시아어 v7의 상점·길드 문구 개선을 포함합니다.
- 게임 EXE의 PDB 경로, 셰이더 메타데이터, 공개 소스의 개발 PC 절대경로를 정리했습니다. 실행 코드와 기존 셰이더/PSO 바이트는 유지합니다.
- 프로그램·한국어 팩·추가 텍스트팩은 {a.version}입니다. 런처 0.3.9와 영어 기반팩 0.3.8을 사용합니다.
- 기존 런처에서 선택한 언어로 플레이/업데이트를 실행하세요. 실제 게임 화면의 전체 잘림/줄 간격 검수는 진행 중입니다.

English
- Adds 154 missing Korean aircraft, equipment and character display strings. The corresponding English/Russian/Spanish strings were checked in the selected release inputs.
- Includes the Python setup detection fix and Russian v7 shop/guild text improvements.
- Removes local build-machine paths from game debug metadata, shader metadata and public source. Executable instructions and shader/PSO payload bytes are unchanged.
- Program, Korean pack and additional text packs: {a.version}. Launcher: 0.3.9; English base pack: 0.3.8.
- Use Play/Update in your existing launcher. Full in-game clipping/line-spacing review remains ongoing.
'''
    (output/'RELEASE-NOTES.txt').write_text(notes,encoding='utf-8')
    source_note=public/'release-baselines'/a.version/'RELEASE-NOTES.txt';source_note.parent.mkdir(parents=True,exist_ok=True);source_note.write_text(notes,encoding='utf-8')
    proof=dict(version=a.version,program_metadata=peproof,shader_metadata=shaderproof,
               korean=dict(strings=154,sha256=digest(korean),changed_members=ko_changed,delta_readback=True,slot_only_changes=True),
               text_packs=text_proofs,all_four_locales_readback=154,public_published=False,game_started=False,
               launcher_version='0.3.9',pending=['User in-game Korean v23 and Russian v7 display confirmation','Full GUI update/recovery check'])
    write(output/'BUILD-RESULT.json',proof)
    write(public/'release-baselines'/a.version/'BUILD.json',proof)
    entries=[]
    for target in sorted(public.rglob('*')):
        if not target.is_file():continue
        name=target.relative_to(public).as_posix()
        if name in original_public and target.read_bytes()==original_public[name]:continue
        if name.startswith(('tools/','native/','vendor/','release-baselines/')):
            entries.append(dict(source=str(target),target=name,sha256=sha(target)))
    manifest=dict(version=a.version,files=[dict(path=e['target'],sha256=e['sha256']) for e in entries])
    write(public/'release-baselines'/a.version/'SOURCE-MANIFEST.json',manifest)
    mf=public/'release-baselines'/a.version/'SOURCE-MANIFEST.json'
    entries.append(dict(source=str(mf),target=mf.relative_to(public).as_posix(),sha256=sha(mf)))
    # Report lives in docs locally; preserve it in the permitted public baseline tree as well.
    doc=public/'release-baselines'/a.version/'four-locale-display-fix-20261115.md';doc.write_bytes(a.display_report.read_bytes())
    entries.append(dict(source=str(doc),target=doc.relative_to(public).as_posix(),sha256=sha(doc)))
    launcher=a.downloads/oldplan['components']['launcher']['artifact']['file'];assert sha(launcher)==oldplan['components']['launcher']['artifact']['sha256']
    # The release pipeline also verifies the original launcher ZIP contents.
    launcher_zip=a.downloads/'Macross30-launcher-0.3.9.zip'
    assert launcher_zip.exists(),'Download the immutable public launcher ZIP before preparing'
    patch=dict(id='display-privacy-'+a.version,component='game',version=a.version,
               title=dict(ko=a.version+' 한국어 표시·배포 정보 정리',en=a.version+' Korean display and privacy cleanup'),
               body=dict(ko='한국어 누락 문구 154개, Python 설치 준비 수정, 러시아어 v7 및 배포 경로 정보 정리. 런처는 0.3.9입니다.',
                         en='154 Korean display strings, Python setup fix, Russian v7, and release path cleanup. Launcher remains 0.3.9.'))
    config=dict(release=a.version,changed=['game','korean'],baseline_metadata=str(baseline),
                baseline_assets=dict(game=str(inputs),korean=str(inputs),english=str(a.downloads),launcher=str(a.downloads)),
                input_assets=dict(game=str(inputs),korean=str(inputs)),locale_assets=str(locales),locale_updates=updates,
                notes=str(output/'RELEASE-NOTES.txt'),patch_notes=[patch],source_files=entries,readme_replacements=readme_replacements)
    write(output/'release-config.json',config)
    result=prepare(config,output/'candidate');write(output/'candidate/VERIFICATION.json',verify(output/'candidate'))
    # Full source snapshot is review-only; source_files specifies exactly what later publication overlays.
    pack(output/'PUBLIC-SOURCE-REVIEW.zip',{p.relative_to(public).as_posix():p.read_bytes() for p in public.rglob('*') if p.is_file()})
    write(output/'CANDIDATE.json',dict(version=a.version,assets=len(result['artifacts']),public_source_overlays=len(entries),
                                    game_started=False,public_published=False))
    print(json.dumps(dict(ok=True,version=a.version,assets=len(result['artifacts']),source_overlays=len(entries))),flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--previous',required=True,type=Path);p.add_argument('--downloads',required=True,type=Path)
    p.add_argument('--original-elf',required=True,type=Path);p.add_argument('--korean-elf',required=True,type=Path)
    p.add_argument('--display-proof',required=True,type=Path);p.add_argument('--display-report',required=True,type=Path)
    p.add_argument('--public-source',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
    p.add_argument('--version',default='0.3.12');main(p.parse_args())
