"""Freeze the released 0.3.3 source subset without moving HEAD or using its index."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
EVIDENCE=ROOT/'native/analysis/release-033-validation'
BASELINE=ROOT/'docs/release-baselines/0.3.3'

def git(*args, data=None, env=None):
    return subprocess.check_output(['git',*args],cwd=ROOT,input=data,env=env)

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def read(path): return json.loads(path.read_text(encoding='utf-8-sig'))
def write(path,value):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')

def preserve():
    head=git('rev-parse','HEAD').decode().strip()
    for ref in ('refs/heads/release/0.3.3','refs/tags/v0.3.3'):
        if subprocess.run(['git','show-ref','--verify','--quiet',ref],cwd=ROOT).returncode==0:
            raise ValueError('Baseline reference already exists: '+ref)
    plan=read(ROOT/'native/subtitle-candidate/release-033-validation-r1/plan.json')
    for name,expected in plan['source_guards'].items():
        if sha(Path(name))!=expected: raise ValueError('Released source changed: '+name)
    checks=read(EVIDENCE/'artifacts/RELEASE-CHECKS.json')
    for row in checks['artifacts']:
        path=EVIDENCE/'artifacts'/row['file']
        if path.stat().st_size!=row['bytes'] or sha(path)!=row['sha256']:
            raise ValueError('Released artifact changed: '+row['file'])
    paths=set()
    # All explicit production recompiles plus the shared headers/dependency sets.
    paths.update(Path(r['source']).relative_to(ROOT).as_posix() for r in plan['compile'])
    paths.update(Path(p).relative_to(ROOT).as_posix() for p in plan['source_guards'] if Path(p).is_relative_to(ROOT) and '/build/' not in p.replace('\\','/'))
    paths.update([
      'native/macross30/diagnostic_log.h','native/macross30/geom_c5b0_capture_contract.h',
      'vendor/ps3recomp-master/CMakeLists.txt','vendor/ps3recomp-master/libs/misc/custom_music.h',
      'vendor/ps3recomp-master/libs/misc/cellSearch.h','vendor/ps3recomp-master/libs/misc/cellMusicUtility.h',
      'vendor/ps3recomp-master/libs/misc/tests/test_cellSearch.c','vendor/ps3recomp-master/libs/misc/tests/test_custom_music.c',
      'vendor/ps3recomp-master/libs/misc/tests/test_avconf_gamma.c',
      'tools/launcher/Launcher.cs','tools/dist/english_data_package.py','tools/dist/split_language_packages.py',
      'tools/dist/stage_current_release_data.py','tools/dist/preserve_release_baseline.py',
      'tools/dist/template/tools/collect-logs.ps1','docs/release-0.3.3.md','docs/release-0.3.2-candidate.md',
      'docs/wiki/audio.md','docs/custom-sound-menu-localization.md','docs/itembox-tabs-localization.md','docs/quest-badges-localization.md',
      'tools/localization/build_english_supplemental_elf.py','tools/localization/finalize_english_preview_v3.py',
      'tools/localization/publish_english_draft.py','tools/localization/stage_english_image_preview.py',
      'tools/localization/build_english_combat_banner_v3.py','tools/localization/prepare_custom_sound_menus.py',
      'tools/localization/prepare_itembox_tabs.py','tools/localization/prepare_quest_badges.py','tools/localization/stage_custom_sound_menus.py'])
    launcher=ROOT/'tools/launcher-python'
    paths.update(p.relative_to(ROOT).as_posix() for p in launcher.iterdir() if p.is_file() and p.suffix in ('.py','.ps1','.txt'))
    paths.update(p.relative_to(ROOT).as_posix() for p in (ROOT/'tools/report-relay').iterdir() if p.is_file() and p.name not in ('.dev.vars',))
    paths.update('tools/tests/'+n for n in ('test_bgm_playback_status.py','test_atrac_pcm_identity.py','test_audio_pcm_add.py',
      'test_game_options_runtime.py','test_custom_music.py','test_diagnostic_log.py','test_log_collection.py'))
    forbidden={'.exe','.dll','.pdb','.obj','.zip','.elf','.iso','.dpapi','.secret','.pyc'}
    for name in paths:
        path=ROOT/name
        if path.suffix.lower() in forbidden or not path.is_file() or path.name.startswith('.dev.vars'):
            raise ValueError('Unexpected release source input: '+name)
    write(BASELINE/'artifacts.json',checks)
    for src,dst in [('install-validation.json','install-validation.json'),('public-backend-verification.json','public-backend-verification.json'),
                    ('public-download-verification.json','public-download-verification.json'),('discord-e2e-verification.json','discord-e2e-verification.json')]:
        write(BASELINE/dst,read(EVIDENCE/src))
    write(BASELINE/'language-inputs.json',{
      'korean_bundle':'quest-badges-menu-v18','korean_ready':read(ROOT/'localization/image_assets/quest-badges-menu-v18/ready.json'),
      'english_ready':read(ROOT/'localization/en-US/pack-ready.json'),
      'full_binary_inputs_in_git':False,'reconstruction':'Use the original ISO and the immutable released delta packs; hashes identify local frozen language inputs.'})
    write(BASELINE/'build-provenance.json',{'source_base_commit':head,'candidate_plan_sha256':sha(ROOT/'native/subtitle-candidate/release-033-validation-r1/plan.json'),
      'candidate_build_result':read(ROOT/'native/subtitle-candidate/release-033-validation-r1/build-result.json'),
      'relay_worker_sha256':sha(ROOT/'tools/report-relay/worker.mjs'),
      'relay_endpoint':'https://macross30-log-relay.saintiron82.workers.dev',
      'public_release':'https://github.com/saintiron82/macross30-releases/releases/tag/v0.3.3',
      'public_metadata_commit':'0a6321c','excluded_post_release_changes':['native/macross30/realtime_step.cpp','native/macross30/realtime_step.h',
      'tools/local_build/ppu_hooks.json','tools/play/play_native.py','tools/diag/check_game_speed.py','tools/tests/test_realtime_step.py','tools/tests/test_game_speed_fix.py'],
      'known_limits':'Generated objects and local language binaries remain external build inputs; no complete original game assets, saves or credentials are added.'})
    inventory={name:{'bytes':(ROOT/name).stat().st_size,'sha256':sha(ROOT/name)} for name in sorted(paths)}
    write(BASELINE/'source-inventory.json',inventory)
    paths.update(p.relative_to(ROOT).as_posix() for p in BASELINE.iterdir() if p.is_file())
    content={name:(ROOT/name).read_bytes() for name in sorted(paths)}
    environment={**os.environ,'GIT_AUTHOR_NAME':'Macross30 Project','GIT_AUTHOR_EMAIL':'macross30-project@example.invalid',
      'GIT_COMMITTER_NAME':'Macross30 Project','GIT_COMMITTER_EMAIL':'macross30-project@example.invalid'}
    with tempfile.TemporaryDirectory(prefix='macross-baseline-index-') as temp:
        environment['GIT_INDEX_FILE']=str(Path(temp)/'index')
        git('read-tree',head,env=environment)
        modes={}
        for row in git('ls-files','--stage').decode().splitlines():
            mode,_,_,name=row.split(None,3); modes[name]=mode
        for name,data in content.items():
            blob=git('hash-object','-w','--stdin','--no-filters',data=data).decode().strip()
            git('update-index','--add','--cacheinfo',modes.get(name,'100644'),blob,name,env=environment)
        tree=git('write-tree',env=environment).decode().strip()
        for name,data in content.items():
            if (ROOT/name).read_bytes()!=data: raise ValueError('Concurrent release source change: '+name)
        commit=git('commit-tree',tree,'-p',head,data=b'Preserve released 0.3.3 game, KO/EN packs, launcher and Discord relay baseline\n',env=environment).decode().strip()
    git('update-ref','refs/heads/release/0.3.3',commit,'0'*40)
    tag_message='Macross 30 released baseline 0.3.3\n\nPrivate source: '+commit+'\nPublic release: https://github.com/saintiron82/macross30-releases/releases/tag/v0.3.3\n\n'
    tag_message+='\n'.join(r['file']+' SHA256 '+r['sha256'] for r in checks['artifacts'])+'\n'
    git('tag','-a','v0.3.3',commit,'-F','-',data=tag_message.encode(),env=environment)
    result={'commit':commit,'branch':'release/0.3.3','tag':'v0.3.3','base':head,'overlaid_file_count':len(content),
            'current_head_unchanged':git('rev-parse','HEAD').decode().strip()==head,'normal_index_untouched':True}
    write(EVIDENCE/'git-baseline.json',result)
    print(json.dumps(result,indent=2))

if __name__=='__main__': preserve()
