"""Stage private 0.4.0 packages from the verified m29/mouse/UI composition."""
import argparse
import copy
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT/'tools/release')]
from check_public_runtime040 import check
from pipeline import identity, prepare, read, sha, verify, write
from prepare_display_privacy_release import encoded, pack, unpack
from privacy import check_owned_bytes, digest, relative_source_paths, sanitize_pe_pdb

OUT = ROOT/'build/releases/0.4.0'
BASE = ROOT/'build/releases/0.3.12/field-display-20261126/candidate/assets'
RUNTIME = OUT/'runtime-ui-nof8-20261126'
VERSION = '0.4.0'


def public_source(config):
    """Freeze explicit authored overlays, separately from uploadable game assets."""
    sources = {r['target']:Path(r['source']) for r in config.get('source_files', [])}
    for name in ['tools/release/merge_runtime040.py','tools/release/build_runtime_ui040.py',
                 'tools/release/check_public_runtime040.py','tools/release/prepare_runtime040_release.py',
                 'tools/release/frame_limit040.py','tools/release/mouse_settings040.py','tools/tests/fps_limit040_fixture.cpp',
                 'tools/localization/build_native_ui040.py','tools/tests/test_runtime040_integration.py',
                 'tools/tests/test_native_ui040.py','tools/tests/native_ui040_fixture.c',
                 'native/macross30/l10n_bridge.cpp','native/macross30/l10n_bridge.h',
                 'tools/tests/test_l10n_csv_identity.cpp','tools/tests/test_l10n_bridge.cpp',
                 'tools/tests/test_release040_packages.py']:
        sources[name] = ROOT/name
    for p in (ROOT/'localization/native-ui/0.4.0').glob('*.tsv'):
        sources['release-baselines/0.4.0/native-ui/'+p.name] = p
    # Preserve exact accepted-parent and merged sources for review; generated game code is excluded.
    parent = Path('build/tests/Macross-real60/m29-lto')
    accepted = read(parent/'build-result.json')
    candidates = {Path(row['path']):row['sha256'] for row in accepted['sources'].values()
                  if '/recompiled/' not in row['path'].replace('\\','/') and not row['path'].endswith('measure_window.cpp')}
    for path, expected in candidates.items():
        assert sha(path) == expected, path.name
        sources['native/release040/accepted-m29/'+path.name] = path
    for p in (RUNTIME/'src').iterdir():
        if p.is_file(): sources['native/release040/merged/'+p.name] = p
    for name in ['delta-time-034-summary-20261125.md','performance-r8-r10-20261125.md','IMPACT-RUMBLE.md']:
        sources['release-baselines/0.4.0/'+name] = OUT/'runtime-reference'/name
    entries = []
    for target, source in sorted(sources.items()):
        data = source.read_bytes()
        # Authored source review is UTF-8; shorten build provenance and machine-specific roots.
        text = data.decode('utf-8-sig')
        text = re.sub(r'(?i)C:[/\\]+Users[/\\]+saint[/\\]+orca[/\\]+workspaces[/\\]+Macross[/\\]+delta-time-034', '.', text)
        text = relative_source_paths(text)
        data = text.encode('utf8')
        check_owned_bytes(data)
        destination = OUT/'public-source'/target
        destination.parent.mkdir(parents=True,exist_ok=True)
        destination.write_bytes(data)
        entries.append(dict(source=str(destination),target=target,sha256=digest(data)))
    snapshot = OUT/'PUBLIC-SOURCE-REVIEW.next.zip'
    pack(snapshot, {r['target']:Path(r['source']).read_bytes() for r in entries})
    snapshot.replace(OUT/'PUBLIC-SOURCE-REVIEW.zip')
    return entries


def main(refresh=False):
    candidate = OUT/('candidate-next' if refresh else 'candidate')
    assert not candidate.exists(), 'Inspect existing candidate rather than overwriting it'
    tests = ROOT/'build/tests/release040'; tests.mkdir(exist_ok=True)
    tmp = tests/'tmp'; tmp.mkdir(exist_ok=True)
    tempfile.tempdir = str(tmp)
    os.environ.update(TEMP=str(tmp),TMP=str(tmp),TMPDIR=str(tmp))
    assert read(ROOT/'build/tests/runtime040-integration/RESULT.json')['passed']
    assert read(ROOT/'build/tests/native-ui040-20261126/UI-RESULT.json')['passed']
    runtime = read(RUNTIME/'BUILD-RESULT.json')
    assert runtime['passed'] and runtime['mouse_merge_complete']
    csv_identity = read(ROOT/'build/tests/release040-playtest/render-regression/CSV-IDENTITY-RESULT.json')
    assert csv_identity['passed'] and csv_identity['actual_release_object']
    assert csv_identity['resources_checked'] == 124
    assert csv_identity['source_sha256'] == sha(RUNTIME/'src/l10n_bridge.cpp')
    assert csv_identity['object_sha256'] == sha(RUNTIME/'objects/l10n_bridge.cpp.obj')
    binary = check(RUNTIME); assert binary['exe_sha256'] == runtime['exe_sha256']
    write(RUNTIME/'PUBLIC-RUNTIME-CHECK.json',binary)
    inputs = OUT/'packages'; inputs.mkdir(exist_ok=True)
    boot = read(BASE/'BOOT-INSTALL.json')
    inventory = read(OUT/'COMPONENTS.json')['components']
    for row in inventory:
        assert sha(ROOT/row['input']['path']) == row['input']['sha256'], row['component']
    files = unpack(BASE/boot['files']['program']['file'])
    before = dict(files)
    exe, proof = sanitize_pe_pdb((RUNTIME/'Macross30Recompiled.exe').read_bytes())
    assert digest(exe) == runtime['exe_sha256'], 'Linked release should already use basename PDB metadata'
    files['program/bin/Macross30Recompiled.exe'] = exe
    guard = json.loads(files['program/bin/assets/locale-base.json'])
    assert guard['runtime_version'] == '0.3.9'
    guard.update(runtime_exe_sha256=digest(exe),runtime_release_version=VERSION)
    guard_bytes = encoded(guard)
    files['program/bin/assets/locale-base.json'] = guard_bytes
    cmd = files['program/base-game/Macross30.cmd'].decode('utf8').replace('\r\n','\n')
    old = 'rem 0.3.4 release: FPS unlocking is under repair and unavailable.\nset "_M30_UNLOCK=0"\nset "MACROSS_FRAME_VBLANKS=2"\nset "MACROSS_DT_STEP=0"'
    assert cmd.count(old) == 1
    cmd = cmd.replace('set "_M30_UNLOCK=0"\n', 'set "_M30_UNLOCK=0"\nset "_M30_LIMIT="\n', 1)
    cmd = cmd.replace('if "%_M30_K%"=="fps_unlock" set "_M30_UNLOCK=%_M30_V%"',
        'if "%_M30_K%"=="fps_unlock" set "_M30_UNLOCK=%_M30_V%"\n'
        'if "%_M30_K%"=="fps_limit" if "%_M30_V%"=="30" set "_M30_LIMIT=30"\n'
        'if "%_M30_K%"=="fps_limit" if "%_M30_V%"=="60" set "_M30_LIMIT=60"\n'
        'if "%_M30_K%"=="fps_limit" if "%_M30_V%"=="0" set "_M30_LIMIT=0"')
    cmd = cmd.replace(old, 'rem Maximum FPS: 30, 60 or unlimited (0); elapsed-time correction stays enabled.\n'
        'set "MACROSS_FPS_LIMIT=30"\nif "%_M30_UNLOCK%"=="1" set "MACROSS_FPS_LIMIT=0"\n'
        'if defined _M30_LIMIT set "MACROSS_FPS_LIMIT=%_M30_LIMIT%"\n'
        'set "MACROSS_FRAME_VBLANKS=2"\nif not "%MACROSS_FPS_LIMIT%"=="30" set "MACROSS_FRAME_VBLANKS=1"\n'
        'set "_M30_UNLOCK=0"\nif "%MACROSS_FPS_LIMIT%"=="0" set "_M30_UNLOCK=1"\n'
        'set "MACROSS_DT_STEP=1"\nset "EDGE_ANIM=1"\nset "EDGE_ANIM_VERIFY=0"\nset "MACROSS_UNLOCKED_PRESENT_VSYNC=0"')
    cmd = cmd.replace('fps_unlock is disabled in this release.', 'fps_limit selects 30, 60 or unlimited; F9 applies it live.')
    cmd = cmd.replace('F9 = options (resolution, fullscreen, vsync, bloom)', 'F9 = options (display, FPS cap, vibration, mouse)')
    files['program/base-game/Macross30.cmd'] = cmd.replace('\n','\r\n').encode()
    recipe = json.loads(files['program/base-game/recipe.json']); recipe['version'] = VERSION
    files['program/base-game/recipe.json'] = encoded(recipe)
    manifest = json.loads(files.pop('program/program-manifest.json')); manifest['version'] = VERSION
    manifest['files'] = [dict(path=n[len('program/'):],bytes=len(data),sha256=digest(data)) for n,data in sorted(files.items())]
    files['program/program-manifest.json'] = encoded(manifest)
    for name, data in files.items(): check_owned_bytes(data)
    assert not re.search(rb'(?i)[A-Z]:[\\/]+Users[\\/]+saint',exe)
    program = inputs/f'Macross30-program-{VERSION}.zip'; pack(program, files)
    boot['files']['program'] = {**identity(program),'exe_sha256':digest(exe)}
    boot['version'] = VERSION
    boot['features']['fps_unlock'] = dict(available=True,status='available',default_cap=30,live_toggle=True,elapsed_time_correction=True)
    boot['features']['fps_limit'] = dict(available=True,choices=[30,60,0],unlimited_value=0,default_cap=30,live_toggle=True,setting='fps_limit',legacy_setting='fps_unlock')
    boot['features']['vibration'] = dict(available=True,strength_percent=[0,200],default_strength_percent=100)
    boot['features']['mouse_control'] = dict(available=True,default_enabled=False,sensitivity=[0.01,8.0],live_toggle=True)
    boot['features']['locale_packs']['runtime_release_version'] = VERSION
    for locale, row in list(boot['language_packs'].items()):
        language = unpack(BASE/row['file'])
        recipe = json.loads(language['payload/recipe.json']); recipe['version'] = VERSION
        language['payload/recipe.json'] = encoded(recipe)
        path = inputs/f'Macross30-language-{locale}-{VERSION}.zip'; pack(path,language)
        boot['language_packs'][locale] = {**row,**identity(path)}
    for locale, row in list(boot['locale_packs'].items()):
        text = unpack(BASE/row['file']); doc = json.loads(text['pack.json'])
        catalog = ROOT/'localization/native-ui/0.4.0'/('en-US-fix.tsv' if locale=='en-US' else locale+'.tsv')
        text[doc['resources']['native_ui']] = catalog.read_bytes()
        doc['source_guards'] = [dict(path='bin/assets/locale-base.json',sha256=digest(guard_bytes))]
        for item in doc['proofs']:
            if item['path'] == doc['resources']['native_ui']: item['sha256'] = digest(catalog.read_bytes())
        text['pack.json'] = encoded(doc)
        path = inputs/f'Macross30-text-{locale}-{VERSION}.zip'; pack(path,text)
        boot['locale_packs'][locale] = {**row,**identity(path),'manifest_sha256':digest(text['pack.json'])}
    write(inputs/'BOOT-INSTALL.json',boot)
    write(OUT/'PE-METADATA-PROOF.json',proof)
    write(OUT/'PACKAGE-CHANGES.json',dict(program_changed_members=sorted(n for n in files if files[n]!=before.get(n)),
        runtime_sha256=digest(exe),language_data_deltas_unchanged=True,game_started=False))
    notes='''Macross 30 0.4.0

한국어
- 통합 경과 시간 처리와 F9 최대 프레임 30 / 60 / 무제한 선택. 실행 중 적용, 설정 저장.
- 피격·다운·격추 및 지면/벽/수면 충돌 진동 추가, F9 세기 0–200%.
- 마우스 오른쪽 스틱·버튼 바인딩 지원. 기본 OFF. 켜기·Y축 반전·X/Y 감도를 F9 → 키/마우스 설정에 통합.
- 한국어 소개 문구 154개와 필드 이름 75종 보완. 마니아동아리 / Mania Club 반영.
- 새 번역 배치 CSV를 읽을 때 전투 화면이 흐린 배경만 남던 문자 변환 오류 수정.
- 러시아어 v7 개선과 영어 보정·스페인어 v3 유지. F9 5개 언어 문구 보완.
- CPU·렌더 경로 최적화, Python 설치 준비 수정, 배포 정보의 개인 경로 정리.
- F8 측정 기능 제거. 기존 HD 텍스처·4K 선택·셰이더 준비·기본 블룸 유지.
- 게임·한국어·영어 기반팩·추가 텍스트팩 0.4.0, 런처 0.3.9 유지.
- 성능은 장면과 PC에 따라 달라지며 4K에서 항상 60 FPS를 보장하지 않습니다.

English
- Measured elapsed-time correction and a saved live F9 maximum FPS selection: 30 / 60 / Unlimited.
- New hit/down/destruction and ground/wall/water impact vibration; F9 strength 0–200%.
- Native mouse right-stick input and button bindings, disabled by default; enable, Y inversion and X/Y sensitivity under F9 → Key / mouse bindings.
- 154 Korean display strings and 75 field names completed; 마니아동아리 / Mania Club.
- Fixed translated placement CSV loading that could leave the battle screen showing only a blurred background.
- Russian v7 improvements, retained English corrections/Spanish v3, and five-language F9 labels.
- CPU/render optimizations, Python setup fix, private build-path cleanup, and removal of F8 measurement.
- Retains HD textures, 4K resolutions, shader preparation and default bloom. Performance varies by scene and PC.
- Game and language packs: 0.4.0. Launcher: 0.3.9.
'''
    (OUT/'RELEASE-NOTES.txt').write_text(notes,encoding='utf8')
    old = read(ROOT/'build/releases/0.3.12/release-config.json')
    source_files = public_source(old)
    config = dict(release=VERSION,changed=['game','korean','english'],baseline_metadata=str(OUT/'baseline'),
        baseline_assets=dict(game=str(BASE),korean=str(BASE),english=str(BASE),launcher=str(ROOT/'build/downloads/release-privacy-20261115')),
        input_assets={key:str(inputs) for key in ['game','korean','english']},locale_assets=str(inputs),locale_updates=boot['locale_packs'],
        notes=str(OUT/'RELEASE-NOTES.txt'),source_files=source_files,patch_notes=[dict(id='game-0.4.0',component='game',version=VERSION,
          title=dict(ko='0.4.0 프레임 제한·진동·마우스·번역',en='0.4.0 FPS cap, vibration, mouse and translations'),
          body=dict(ko='F9 최대 프레임 30/60/무제한, 새 진동과 마우스 설정, 성능·번역 개선. 런처 0.3.9 유지.',en='Live maximum FPS: 30/60/Unlimited, new vibration and mouse controls, performance and translation improvements. Launcher remains 0.3.9.'))],
        readme_addendum={'README.md':'**0.4.0:** F9 최대 프레임 30/60/무제한·진동·마우스 조작, 성능 및 번역 개선. 게임·언어팩 0.4.0 / 런처 0.3.9.',
                         'README.en.md':'**0.4.0:** Live maximum FPS: 30/60/Unlimited, vibration and mouse controls, performance and translation updates. Game/language packs 0.4.0; launcher 0.3.9.'})
    write(OUT/'release-config.json',config)
    result = prepare(config,candidate)
    write(candidate/'VERIFICATION.json',verify(candidate))
    print(json.dumps(dict(passed=True,upload_assets=len(result['artifacts']),versions={k:r['version'] for k,r in result['components'].items()},published=False)),flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--refresh',action='store_true')
    main(parser.parse_args().refresh)
