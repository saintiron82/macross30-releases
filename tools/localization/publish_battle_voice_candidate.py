"""Select the checked native voice-caption build; activation remains explicit."""
import json
import os
import argparse
from pathlib import Path

from stage_ui_images import sha_file

ROOT=Path(__file__).resolve().parents[2]
BASE=ROOT/'native/subtitle-candidate'
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--candidate-dir',default='build-20261028-battle-voices')
parser.add_argument('--live-proof',type=Path)
parser.add_argument('--inherit-live-proof',type=Path,help='reuse unchanged runtime proof for a catalog-only build; does not claim the new EXE was played')
parser.add_argument('--previous-candidate-dir')
args=parser.parse_args()
assert not(args.live_proof and args.inherit_live_proof),'Choose exact live proof or data-only inheritance'
B=(BASE/args.candidate_dir).resolve()
assert B.parent==BASE.resolve(),'Candidate directory escaped native/subtitle-candidate'
load=lambda p:json.loads(p.read_text(encoding='utf8'))
result=load(B/'build-result.json');plan=load(B/'plan.json')
assert result['status']=='compiled_and_linked_candidate_only'
for path,digest in result['original_outputs'].items():assert sha_file(Path(path))==digest,'Normal build changed'
for path,digest in plan['source_guards'].items():assert sha_file(Path(path))==digest,'Frozen source changed'
bridge=load(ROOT/'native/analysis/battle-voice-subtitle-bridge-check/result.json')
assert bridge['status']=='PASS' and bridge['production_bridge_sha256']==sha_file(ROOT/'native/macross30/battle_voice_subtitles.cpp')
catalog_test=load(ROOT/'native/analysis/battle-voice-catalog-check/result.json')
assert catalog_test['status']=='PASS'
assert load(ROOT/'native/analysis/battle-subtitle-visibility-check/result.json')['status']=='PASS'
exe=B/'Macross30Recompiled.exe';assert sha_file(exe)==result['products']['exe']['sha256']
catalog=load(ROOT/'localization/subtitles/battle-voice/catalog-build.json')
assert catalog['generated_sha256']==sha_file(ROOT/'native/macross30/battle_voice_catalog.inc')
assert catalog_test['generated_catalog_sha256']==catalog['generated_sha256']
assert catalog_test['nonempty_stems']==catalog['resolved_stems']
if catalog.get('Medium_reviewed_sha256'):
    review_root=ROOT/'localization/subtitles/battle-voice/medium-crosscheck-v1/candidate-review-v1'
    assert sha_file(review_root/'translation-reviewed.json')==catalog['Medium_reviewed_sha256']
    assert sha_file(review_root/'candidates.json')==catalog['Medium_candidate_source_sha256']
if catalog.get('lexical_reviewed_sha256'):
    batches=catalog.get('lexical_batches') or [dict(version='lexical-review-v2',reviewed_sha256=catalog['lexical_reviewed_sha256'],candidate_source_sha256=catalog['lexical_candidate_source_sha256'])]
    for batch in batches:
        version=batch['version']
        assert version.startswith('lexical-review-v') and version[len('lexical-review-v'):].isdigit()
        review_root=ROOT/'localization/subtitles/battle-voice/medium-crosscheck-v1'/version
        assert sha_file(review_root/'translation-reviewed.json')==batch['reviewed_sha256']
        assert sha_file(review_root/'candidates.json')==batch['candidate_source_sha256']
font_bundle=None
if catalog.get('required_font_ABF_sha256'):
    from install_korean_ui import bundle_targets, preflight
    drafts=load(ROOT/'localization/subtitles/battle-voice/asr-caption-drafts.json')
    assert sha_file(ROOT/'localization/subtitles/battle-voice/asr-caption-drafts.json')==catalog['ASR_drafts_sha256']
    font_bundle=drafts['required_font_bundle']
    assert load(ROOT/'localization/image_assets/active-ui.json')['bundle']==font_bundle
    font_folder=ROOT/'localization/image_assets'/font_bundle
    authority=load(font_folder/'candidate/authority.json')
    font=next(row for row in authority['entries'] if row['type']=='ABF')
    assert font['replacement_sha256']==catalog['required_font_ABF_sha256']
    assert preflight(bundle_targets(font_folder))==[], 'Required font is not installed'
live=False;runtime_verified=False;inherited=None
if args.live_proof:
    proof_path=args.live_proof.resolve()
    assert proof_path.is_relative_to(ROOT/'native/analysis')
    proof=load(proof_path)
    assert proof['status']=='GENERIC_BRIDGE_LIVE_VALIDATED'
    assert proof['candidate_sha256']==sha_file(exe)
    assert all(proof[k] is True for k in ('reaction_display_verified','reaction_cleared_verified','original_story_priority_verified','test_processes_closed','player_checkpoint_unchanged'))
    for row in proof['evidence']:
        path=(ROOT/row['path']).resolve()
        assert path.is_relative_to(ROOT) and sha_file(path)==row['sha256']
    live=True;runtime_verified=True
if args.inherit_live_proof:
    from data_only_subtitle_proof import inherit
    assert args.previous_candidate_dir,'Data-only inheritance requires previous candidate directory'
    previous=(BASE/args.previous_candidate_dir).resolve();assert previous.parent==BASE.resolve()
    inherited=inherit(previous,B,args.inherit_live_proof)
    assert inherited['catalog_generated_sha256']==catalog['generated_sha256']
    assert inherited['bridge_runtime_inherited_verified'] and inherited['candidate_live_game_verified'] is False
    for name,source in [('catalog-test.json',ROOT/'native/analysis/battle-voice-catalog-check/result.json'),('catalog-build.json',ROOT/'localization/subtitles/battle-voice/catalog-build.json')]:
        snapshot=B/name;raw=source.read_bytes()
        if snapshot.exists():assert snapshot.read_bytes()==raw,'Preserve differing publication snapshot'
        else:snapshot.write_bytes(raw)
    inherited['catalog_test_checked']=True
    inherited['catalog_test_snapshot']=dict(path=str(B/'catalog-test.json'),sha256=sha_file(B/'catalog-test.json'))
    inherited['catalog_build_snapshot']=dict(path=str(B/'catalog-build.json'),sha256=sha_file(B/'catalog-build.json'))
    (B/'data-only-runtime-proof.json').write_text(json.dumps(inherited,indent=2)+'\n',encoding='utf8')
    runtime_verified=True
doc=dict(status='ready',candidate=exe.relative_to(BASE).as_posix(),candidate_sha256=sha_file(exe),
    base_exe_sha256=result['original_outputs'][str(ROOT/'native/build/Macross30Recompiled.exe')],
    dependencies=[dict(name='SDL2.dll',sha256=sha_file(B/'SDL2.dll'))],
    content_status=f'Native post-selection voice subtitle bridge; {catalog["resolved_stems"]} exact stems; explicit MACROSS_BATTLE_SUBTITLES=1 until live verification',
    battle_bridge_enabled_by_default=runtime_verified,live_game_verified=live,
    bridge_live_game_verified=runtime_verified,candidate_live_game_verified=live)
if font_bundle:doc['required_font_bundle']=font_bundle
if live:
    doc['content_status']=f'Native voice subtitle bridge; {catalog["resolved_stems"]} exact stems; generic reaction display/clear and original-story priority verified in game; all dialogue listening review incomplete'
    doc['live_proof']=str(proof_path.relative_to(ROOT))
if inherited:
    doc['content_status']=f'Catalog-only update: {catalog["resolved_stems"]} stems independently checked; unchanged runtime inherits prior live proof; this EXE not played'
    doc['inherited_runtime_proof']=str((B/'data-only-runtime-proof.json').relative_to(ROOT))
pointer=BASE/'active.json';before=B/'previous-active.json'
if not before.exists():before.write_bytes(pointer.read_bytes())
tmp=pointer.with_suffix('.voice-candidate.tmp');tmp.write_text(json.dumps(doc,indent=2)+'\n',encoding='utf8');os.replace(tmp,pointer)
print(json.dumps(dict(ready=True,exe=str(exe),sha256=doc['candidate_sha256'],bridge_default=runtime_verified,candidate_live_game_verified=live)))
