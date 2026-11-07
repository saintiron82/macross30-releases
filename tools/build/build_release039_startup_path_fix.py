"""Link one host-path fix over the immutable public 0.3.9 object set."""
import json
from pathlib import Path
import shutil

import build_subtitle_candidate as build

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT/'native/subtitle-candidate/release-039-integrated-r1'
OUT = ROOT/'native/subtitle-candidate/release-039-startup-path-fix-r2'


def main():
    if OUT.exists():
        raise ValueError('Candidate output must be fresh')
    assert build.sha(BASE/'Macross30Recompiled.exe') == '8688e4bb3f5d5be6744ef8b21c8f13633ee87192f533683a8934f209b5d0843a'
    previous = json.loads((BASE/'plan.json').read_text(encoding='utf-8'))
    for name, expected in previous['public_baseline']['libraries_sha256'].items():
        assert build.sha(Path(name)) == expected, name
    OUT.mkdir(parents=True)
    source = ROOT/'vendor/ps3recomp-master/libs/system/cellGame.c'
    frozen = OUT/'frozen/cellGame.c'
    frozen.parent.mkdir()
    shutil.copyfile(source, frozen)
    headers = list((ROOT/'vendor/ps3recomp-master/include').rglob('*.h'))
    headers += list((ROOT/'vendor/ps3recomp-master/runtime/ppu').glob('*.h'))
    headers += [source.with_suffix('.h')]
    plan = build.plan(OUT, [source], [frozen, *headers])
    row = plan['compile'][0]
    row.update(source=str(frozen), source_sha256=build.sha(frozen))
    row['command'][-1] = str(frozen)
    row['command'].insert(1, '/I'+str(source.parent))
    objects = previous['response_objects']
    matches = [name for name in objects if Path(name).name.endswith('cellGame.c.obj')]
    assert len(matches) == 1, matches
    old_object = matches[0]
    retained = {name: build.sha(Path(name)) for name in objects if name != old_object}
    plan['response_objects'] = [row['output'] if name == old_object else name for name in objects]
    plan['response_libraries'] = previous['response_libraries']
    plan['reused_object_stamps'] = {name: [Path(name).stat().st_size, Path(name).stat().st_mtime_ns] for name in retained}
    plan['public_baseline'] = dict(version='0.3.9', exe_sha256=build.sha(BASE/'Macross30Recompiled.exe'),
        retained_sha256=retained, replaced_object=old_object, libraries_sha256=previous['public_baseline']['libraries_sha256'],
        change='cellGame host buffers separated from 128-byte guest ABI', recompiled_source_count=1)
    (OUT/'plan.json').write_text(json.dumps(plan, indent=2)+'\n', encoding='utf-8')
    result = build.execute(plan)
    assert all(build.sha(Path(name)) == expected for name, expected in retained.items())
    (OUT/'build-result.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(dict(status=result['status'], product=result['products']['exe'],
        retained_objects=len(retained), game_started=False, public_published=False)))


if __name__ == '__main__':
    main()
