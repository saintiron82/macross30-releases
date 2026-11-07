"""Select the validated isolated build for the next normal launch only."""
import hashlib
import json
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
base = ROOT/'native/subtitle-candidate'
folder = base/'build-20261027-battle-option'
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
result = json.loads((folder/'build-result.json').read_text(encoding='utf8'))
plan = json.loads((folder/'plan.json').read_text(encoding='utf8'))
assert result['status'] == 'compiled_and_linked_candidate_only'
assert all(result[k] for k in ('original_outputs_preserved','reused_objects_unchanged','frozen_sources_unchanged'))
for name, digest in result['original_outputs'].items():
    assert sha(Path(name)) == digest, 'Normal build changed: '+name
for name, digest in plan['source_guards'].items():
    assert sha(Path(name)) == digest, 'Source changed: '+name
for fixture in ('native/analysis/battle-subtitle-visibility-check/result.json',
                'localization/subtitles/option-fixture/result.json'):
    assert json.loads((ROOT/fixture).read_text(encoding='utf8'))['status'] == 'PASS'
exe = folder/'Macross30Recompiled.exe'
assert sha(exe) == result['products']['exe']['sha256']
assert '자막 표시 (영상·전투)'.encode('utf-16le') in exe.read_bytes()
doc = dict(status='ready', candidate=exe.relative_to(base).as_posix(),
           candidate_sha256=sha(exe),
           base_exe_sha256=result['original_outputs'][str(ROOT/'native/build/Macross30Recompiled.exe')],
           dependencies=[dict(name='SDL2.dll',sha256=sha(folder/'SDL2.dll'))],
           content_status='Shared movie/battle caption visibility option; new reaction-voice Lua activation NOT installed',
           live_game_verified=False)
pointer = base/'active.json'
backup = folder/'previous-active.json'
if not backup.exists() and pointer.exists():
    backup.write_bytes(pointer.read_bytes())
temporary = base/'active.battle-option.tmp'
temporary.write_bytes((json.dumps(doc,ensure_ascii=False,indent=2)+'\n').encode('utf8'))
os.replace(temporary,pointer)
print(json.dumps(doc,ensure_ascii=False,indent=2))
