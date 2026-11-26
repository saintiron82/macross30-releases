"""Merge the reviewed mouse changes into the accepted m29 pad/options sources."""
import difflib
import hashlib
import json
from pathlib import Path
import re
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'build/releases/0.3.12/display-privacy-20261115/public-source/vendor/ps3recomp-master/libs'
PARENT = Path('build/tests/Macross-real60/m29-lto/src/rumble')
OUT = ROOT / 'build/releases/0.4.0/merged-source'
MOUSE = ROOT / 'build/releases/0.4.0/MOUSE-SOURCE-REVIEW.zip'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    assert sha(MOUSE.read_bytes()) == 'c229f6a8f00a25bfc90d394e96b01e4058f5dae21b77684eee09f6c709722066'
    OUT.mkdir(exist_ok=True)
    expected = {'cellPad.c': 'b529982265570232148e2ee79e3b0cbe3cb8107cc9ee43c29747374048eb5fb0',
                'rsx_null_backend.c': 'd2a07ae3048298ba59f9fec8bcaa3dee1621c28e959d928e68f7716c9479b155'}
    proof = {}
    with zipfile.ZipFile(MOUSE) as z:
        for group, name in [('input', 'cellPad.c'), ('video', 'rsx_null_backend.c')]:
            assert sha((PARENT/name).read_bytes()) == expected[name]
            common = (BASE/group/name).read_text('utf8')
            accepted = (PARENT/name).read_text('utf8')
            for line in common.splitlines():
                if line.startswith('#include "') and '../' in line:
                    inc = line.split('"')[1]
                    accepted = accepted.replace('"'+Path(inc).name+'"', '"'+inc+'"')
            mouse = z.read('vendor/ps3recomp-master/libs/'+group+'/'+name).decode('utf8')
            if name == 'cellPad.c':
                a, b = common.splitlines(True), mouse.splitlines(True)
                selected = []
                for tag, i, j, k, l in difflib.SequenceMatcher(None, a, b).get_opcodes():
                    changed = ''.join(a[i:j]+b[k:l])
                    rumble = any(t in changed for t in ['s_test_rumble', 'pad_test_rumble_active',
                        'pad_set_test_rumble_active', 'vib.wLeftMotorSpeed', 'const PadRumble want',
                        'if (!s_pad_initialized || !on', 'static PadRumble saved', 'active = on'])
                    selected += a[i:j] if tag == 'equal' or rumble else b[k:l]
                mouse = ''.join(selected)
            c, m, target = OUT/(name+'.common'), OUT/(name+'.mouse'), OUT/name
            c.write_text(common, encoding='utf8')
            m.write_text(mouse, encoding='utf8')
            target.write_text(accepted, encoding='utf8')
            result = subprocess.run(['git', 'merge-file', str(target), str(c), str(m)], capture_output=True)
            assert result.returncode == (0 if name == 'cellPad.c' else 8), result.stderr
            merged = target.read_text('utf8')
            if name == 'rsx_null_backend.c':
                conflicts = list(re.finditer(r'^<<<<<<<[^\n]*\n(.*?)^=======\n(.*?)^>>>>>>>[^\n]*\n', merged, re.M|re.S))
                assert len(conflicts) == 8
                def resolve(match):
                    i = next(i for i, c in enumerate(conflicts) if c.start() == match.start())
                    ours, theirs = match[1], match[2]
                    if i in (0, 1, 7): return ours + theirs
                    if i == 2: return ours + theirs[theirs.index('    fprintf(f, "mouse_enabled='):]
                    if i == 3:
                        return ours.replace('ID_OPT_RUMBLE_STRENGTH_LABEL };',
                            'ID_OPT_RUMBLE_STRENGTH_LABEL, ID_OPT_MOUSE, ID_OPT_MOUSE_INVERT, ID_OPT_MOUSE_X, ID_OPT_MOUSE_Y };')
                    if i == 4: return ours.replace('id == ID_OPT_RUMBLE)', 'id == ID_OPT_RUMBLE || id == ID_OPT_MOUSE || id == ID_OPT_MOUSE_INVERT)')
                    if i == 5: return theirs
                    if i == 6:
                        # Fit rumble and mouse controls into the reviewed 700-pixel window.
                        buttons = ours
                        for old, new in [(426,400),(466,440),(506,480)]:
                            buttons = buttons.replace(', '+str(old)+',', ', '+str(new)+',')
                        controls = theirs[theirs.index('    OPT_CTL(L"BUTTON", macross_ui_string("mouse.enable")'):]
                        for old,new in [(510,520),(545,550),(568,573),(600,607),(623,630)]:
                            controls = controls.replace(', '+str(old)+',', ', '+str(new)+',')
                        return buttons + controls
                    raise AssertionError(i)
                merged = re.sub(r'^<<<<<<<[^\n]*\n(.*?)^=======\n(.*?)^>>>>>>>[^\n]*\n', resolve, merged, flags=re.M|re.S)
                merged = merged.replace(', 312, 420, 110, 0)', ', 312, 420, 80, 0)')
            # Restore staged-source include layout, independently of mutable worktree headers.
            merged = re.sub(r'(#include ")(?:\.\./)+[^"\n]*/([^/"\n]+)(")', r'\1\2\3', merged)
            assert '<<<<<<<' not in merged and '>>>>>>>' not in merged
            target.write_text(merged, encoding='utf8')
            diff = ''.join(difflib.unified_diff((PARENT/name).read_text('utf8').splitlines(True),
                merged.splitlines(True), fromfile='accepted-m29/'+name, tofile='release040/'+name))
            (OUT/(name+'.patch')).write_text(diff, encoding='utf8')
            proof[name] = dict(parent_sha256=expected[name], merged_sha256=sha(target.read_bytes()))
        (OUT/'pad_mouse.h').write_bytes(z.read('vendor/ps3recomp-master/libs/input/pad_mouse.h'))
    proof['pad_mouse.h'] = dict(merged_sha256=sha((OUT/'pad_mouse.h').read_bytes()))
    from frame_limit040 import stage
    stage(OUT, PARENT.parent)
    from mouse_settings040 import move_sensitivity
    options=OUT/'rsx_null_backend.c'
    options.write_text(move_sensitivity(options.read_text('utf8')),encoding='utf8')
    for name in ['rsx_null_backend.c','frame_pacing.cpp','macross_fps_limit.h']:
        proof.setdefault(name,{})['merged_sha256'] = sha((OUT/name).read_bytes())
    (OUT/'rsx_null_backend.c.patch').write_text(''.join(difflib.unified_diff(
        (PARENT/'rsx_null_backend.c').read_text('utf8').splitlines(True),
        (OUT/'rsx_null_backend.c').read_text('utf8').splitlines(True),
        fromfile='accepted-m29/rsx_null_backend.c',tofile='release040/rsx_null_backend.c')),encoding='utf8')
    (OUT/'MERGE-RESULT.json').write_text(json.dumps(dict(passed=True, files=proof,
        rumble_implementation_preserved=True, game_started=False), indent=2)+'\n', encoding='utf8')
    print(json.dumps(proof, indent=2))


if __name__ == '__main__':
    main()
