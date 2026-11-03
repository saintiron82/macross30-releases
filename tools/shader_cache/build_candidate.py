"""Private candidate build: frozen seed-v2 source + shader-prep hooks.

Recompiles only rsx_live_draw.c (with the hooks from patch_live_draw.py and the
two rsx_shader_prep headers) and relinks it with the exact link inputs of the
frozen seed-v2 candidate, so the result differs from the benchmark baseline
only by this work. Also builds MacrossShaderPrep.exe and the C self-test.

    python tools/shader_cache/build_candidate.py --out <new private dir>
"""
import argparse
import hashlib
import json
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import patch_live_draw  # noqa: E402
import toolchain  # noqa: E402

MEASURE = Path('D:/Game/Macross-0.3.5-measurement-20261101')
BASE = MEASURE / 'shader-seed-v2-candidate-20261103'
VIDEO = Path('vendor/ps3recomp-master/libs/video')
HEADERS = ['rsx_shader_prep.h', 'rsx_shader_prep_runtime.h']


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def run(cmd, log, env):
    z = subprocess.run(cmd, env=env, capture_output=True, text=True, encoding='utf-8', errors='replace')
    Path(log).write_text(' '.join(map(str, cmd)) + '\n' + z.stdout + z.stderr, encoding='utf-8')
    if z.returncode:
        raise SystemExit(f'failed ({z.returncode}), see {log}\n' + (z.stdout + z.stderr)[-4000:])
    return z


def link_inputs(rsp_text):
    files = [x.strip('"') for x in shlex.split(rsp_text, posix=False)]
    return [f for f in files if f.lower().endswith(('.obj', '.lib', '.res')) and Path(f).is_absolute()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--tools-only', action='store_true', help='build prep tool and self-test only')
    a = ap.parse_args()
    out = a.out.resolve()
    if not str(out).lower().startswith(str(MEASURE).lower()):
        raise SystemExit('candidate output must be a private directory under ' + str(MEASURE))
    if (out / 'Macross30Recompiled.exe').exists():
        raise SystemExit('refusing to overwrite an existing candidate executable: ' + str(out))
    missing = toolchain.missing()
    if missing:
        raise SystemExit('toolchain missing: ' + ', '.join(missing))
    out.mkdir(parents=True, exist_ok=True)
    env = toolchain.environment()
    t0 = time.monotonic()
    base = json.loads((BASE / 'build-result.json').read_text())
    if sha(BASE / 'Macross30Recompiled.exe') != base['exe_sha256']:
        raise SystemExit('frozen base executable does not match its build-result')

    src = out / 'source'
    if not src.exists():
        shutil.copytree(BASE / 'source', src)
    live = src / VIDEO / 'rsx_live_draw.c'
    frozen_sha = sha(BASE / 'source' / VIDEO / 'rsx_live_draw.c')
    if frozen_sha != base['source_sha256']:
        raise SystemExit('frozen rsx_live_draw.c does not match base build-result')
    raw = (BASE / 'source' / VIDEO / 'rsx_live_draw.c').read_bytes().decode('utf-8').replace('\r\n', '\n')
    live.write_bytes(patch_live_draw.patch(raw).replace('\n', '\r\n').encode('utf-8'))
    for h in HEADERS:
        shutil.copy2(ROOT / VIDEO / h, src / VIDEO / h)
    tool_src = src / 'native/shader_prep'
    tool_src.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / 'native/macross30/shader_prep_main.c', tool_src / 'shader_prep_main.c')
    shutil.copy2(ROOT / 'native/macross30/shader_prep_selftest.c', tool_src / 'rsp_selftest.c')
    # The self-test compiles the real patched loader/wrapper functions.
    patched = live.read_bytes().decode('utf-8').replace('\r\n', '\n')
    s0 = patched.index('static u32 shader_disk_cache_stage_index(')
    s1 = patched.index('static u64 ld_hash_structural_render_state(')
    (tool_src / 'rsp_loader_slice.inc').write_text(
        '/* Generated from the patched rsx_live_draw.c; do not edit. */\n' + patched[s0:s1], encoding='utf-8')

    record = dict(status='private_candidate_built', published=False, shared_checkout_build=False,
                  base_dir=str(BASE), base_exe_sha256=base['exe_sha256'], base_source_sha256=base['source_sha256'])
    cc = toolchain.clang_cl()
    common = [cc, '/nologo', '/O2', '/MD', '/W3', '/D_CRT_SECURE_NO_WARNINGS', '-Wno-deprecated-declarations',
              '/I' + str(src / VIDEO)]
    run(common + [str(tool_src / 'shader_prep_main.c'), '/Fe' + str(out / 'MacrossShaderPrep.exe'),
                  '/Fo' + str(out / 'shader_prep_main.obj'), '/link', 'd3d12.lib', 'dxgi.lib', 'd3dcompiler.lib'],
        out / 'prep-tool-build.log', env)
    run(common + [str(tool_src / 'rsp_selftest.c'), '/Fe' + str(out / 'rsp_selftest.exe'),
                  '/Fo' + str(out / 'rsp_selftest.obj'), '/link', 'd3d12.lib', 'dxgi.lib', 'd3dcompiler.lib'],
        out / 'selftest-build.log', env)
    record['prep_tool_sha256'] = sha(out / 'MacrossShaderPrep.exe')
    record['selftest_sha256'] = sha(out / 'rsp_selftest.exe')

    if not a.tools_only:
        plan = json.loads((ROOT / 'final/runtime/candidates/aircraft-hd-034-v3/plan.json').read_text())
        cp = next(x for x in plan['compile'] if x['source'].endswith('rsx_live_draw.c'))
        obj = out / 'rsx_live_draw.c.obj'
        cmd = []
        for arg in cp['command']:
            if arg.startswith('/Fo'):
                arg = '/Fo' + str(obj)
            elif arg.startswith('/Fd'):
                arg = '/Fd' + str(out / 'compile.pdb')
            elif arg.startswith('-I'):
                arg = arg.replace(str(ROOT), str(src))
            elif arg == cp['source']:
                arg = str(live)
            cmd.append(arg)
        if '/showIncludes' in cmd:
            cmd.remove('/showIncludes')
        run(cmd, out / 'compile.log', env)
        rsp = (BASE / 'link.rsp').read_text(encoding='utf-8')
        old = MEASURE / 'shader-seed-candidate-20261103/rsx_live_draw.c.obj'
        if rsp.count(str(old)) != 1:
            raise SystemExit('base link.rsp does not reference the expected renderer object once')
        inputs = link_inputs(rsp)
        newest = max(Path(f).stat().st_mtime for f in inputs if Path(f) != old)
        base_time = (MEASURE / 'shader-seed-candidate-20261103/Macross30Recompiled.exe').stat().st_mtime
        if newest > base_time:
            raise SystemExit('a link input changed after the frozen base was linked; refusing mixed build')
        rsp = rsp.replace(str(old), str(obj))
        (out / 'link.rsp').write_text(rsp, encoding='utf-8')
        link_plan = json.loads((Path('D:/Game/Macross-034-source-reference-20261101') / 'plan.json').read_text())
        run([x.replace(link_plan['output'], str(out)) for x in link_plan['link']], out / 'link.log', env)
        for name in ['SDL2.dll']:
            shutil.copy2(BASE / name, out / name)
        for name in ['CustomMusic']:
            if (BASE / name).exists() and not (out / name).exists():
                shutil.copytree(BASE / name, out / name)
        (out / 'assets').mkdir(exist_ok=True)
        shutil.copy2(BASE / 'assets/aircraft-hd.pack', out / 'assets/aircraft-hd.pack')
        record['exe_sha256'] = sha(out / 'Macross30Recompiled.exe')
        record['link_inputs'] = {f: sha(f) for f in inputs if Path(f) != old}
        record['link_inputs_unchanged_since_base_link'] = True
    record['source_files'] = {str(p.relative_to(src)).replace('\\', '/'): sha(p)
                              for p in sorted(src.rglob('*')) if p.is_file()}
    record['source_sha256'] = sha(live)
    record['hooks'] = 'tools/shader_cache/patch_live_draw.py applied to frozen rsx_live_draw.c'
    record['seconds'] = round(time.monotonic() - t0, 1)
    (out / 'build-result.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in record.items() if k not in ('source_files', 'link_inputs')}, indent=2))


if __name__ == '__main__':
    main()
