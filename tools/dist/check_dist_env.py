"""Check that the QA launcher (Macross30.cmd) reproduces tools/play/play_native.py.

The real tools/play/play_native.py runs inside a throw-away fake workspace with
subprocess.Popen intercepted, so nothing is launched and the live workspace
(runner lock, native/play logs) is never touched. The generated
Macross30.cmd then really launches a stub executable that records its
environment, arguments and working directory. Both are compared after mapping
the workspace paths to their distribution equivalents, for several
settings.ini cases and a polluted parent environment.

  python tools/dist/check_dist_env.py [--dist C:/GameDev/Macross-dist/Macross30]
"""
import argparse
import contextlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import types
from release_profile import STABLE_ENV, NORMALIZED_ENV
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
BASELINE = Path('native/analysis/frame-rate-priority/wait-owner-clean-proof/process.json')
POWERSHELL = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32/WindowsPowerShell/v1.0/powershell.exe'

# play_native.py workspace path -> distribution path (relative to each root).
PATH_MAP = {
    'native/state-ko-full/hdd0': 'data/hdd0',
    'native/state-ko-full/hdd1': 'data/hdd1',
    'runtime/rpcs3/dev_flash': 'firmware/dev_flash',
    'build/game-ko-full': 'data/disc',
    'runtime/rpcs3': 'bin/ffmpeg',
    'native/play/spu-misses': 'play/spu-misses',
    'native/play': 'play',
    'native/build/Macross30Recompiled.exe': 'bin/Macross30Recompiled.exe',
    'EBOOT.elf': 'bin/EBOOT.ko.elf',
    'localization/subtitles/runtime': 'bin/subtitles',
}

# Parent-environment pollution both launchers must handle the same way.
POLLUTION = {
    'MACROSS_POLLUTED': '1', 'LD_RENDER_SCALE': '9', 'PERF': '1', 'PERF_KEEP': 'kept',
    'SCREEN_AS_TEX': '1', 'FP_CONSTBUF': '1', 'PS3_HDD0_ROOT': r'X:\bogus', 'SPU_DUMP_MISS': r'X:\bogus',
    'PAD_FILE': r'X:\pad.txt', 'CELLMARK_DUMP': '9', 'D3D12_DBG': '1', 'RTT_SAVERT': '1',
    'UNRELATED_KEEP': 'yes',
    'EDGE_SKIN': '1', 'EDGE_ANIM': '1', 'RIBBON_NATIVE': '1',
}

SETTINGS_CASES = {
    'no settings.ini': None,
    'workspace native/play/settings.ini': 'WORKSPACE',
    'scale 1 + fps unlock': 'render_scale=1\nfullscreen=0\nvsync=1\nfps_unlock=1\nbloom=1\n',
    'spaces around =': '  render_scale = 1.25 \r\n fps_unlock = 0 \r\n',
    'invalid scale': 'render_scale=3\nfps_unlock=yes\n',
    'last value wins': 'render_scale=2\nfps_unlock=1\nrender_scale=1.5\nfps_unlock=0\n',
    'scale 2 only': 'render_scale=2\n',
}

STUB_SOURCE = r'''
using System; using System.Collections; using System.IO; using System.Text;
public static class Program {
    public static int Main(string[] args) {
        StringBuilder sb = new StringBuilder();
        sb.Append("CWD\t").Append(Environment.CurrentDirectory).Append('\n');
        sb.Append("ARG\t").Append(Environment.GetCommandLineArgs()[0]).Append('\n');
        foreach (string a in args) sb.Append("ARG\t").Append(a).Append('\n');
        foreach (DictionaryEntry e in Environment.GetEnvironmentVariables())
            sb.Append("ENV\t").Append((string)e.Key).Append('\t').Append(((string)e.Value).Replace("\r", "\\r").Replace("\n", "\\n")).Append('\n');
        byte[] bytes = Encoding.UTF8.GetBytes(sb.ToString());
        using (Stream o = Console.OpenStandardOutput()) o.Write(bytes, 0, bytes.Length);
        Console.Error.WriteLine("stub stderr");
        return args.Length == 2 ? 0 : 3;
    }
}
'''


def play_native_launch(settings_text, base_env):
    """Run the real tools/play/play_native.py in a fake workspace; return (env, command, cwd, fake_root)."""
    fake = Path(tempfile.mkdtemp(prefix='m30-play-native-')).resolve()
    (fake / 'tools').mkdir()
    exe = fake / 'native/build/Macross30Recompiled.exe'
    exe.parent.mkdir(parents=True)
    exe.write_bytes(b'')
    (fake / 'build/game-ko-full').mkdir(parents=True)
    (fake / BASELINE).parent.mkdir(parents=True)
    shutil.copy2(ROOT / BASELINE, fake / BASELINE)
    (fake / 'native/play').mkdir(parents=True)
    if settings_text is not None:
        (fake / 'native/play/settings.ini').write_bytes(settings_text.encode('utf-8'))
    elf = fake / 'EBOOT.elf'
    elf.write_bytes(b'')
    captured = {}

    class Child:
        pid = 0
        def wait(self): return 0
        def terminate(self): pass

    def popen(command, cwd=None, env=None, **_):
        captured.update(command=[str(c) for c in command], cwd=str(cwd), env=dict(env))
        return Child()

    stub = types.ModuleType('localization_launch')
    stub.prepare_localization = lambda root: elf
    binary_selector = types.ModuleType('native_binary_launch')
    selector_source = ROOT / 'tools/play/native_binary_launch.py'
    exec(compile(selector_source.read_text(encoding='utf-8'), str(selector_source), 'exec'), binary_selector.__dict__)
    performance = types.ModuleType('performance_mode')
    performance_source = ROOT / 'tools/play/performance_mode.py'
    exec(compile(performance_source.read_text(encoding='utf-8'), str(performance_source), 'exec'), performance.__dict__)
    script = ROOT / 'tools/play/play_native.py'
    code = compile(script.read_text(encoding='utf-8'), str(fake / 'tools/play/play_native.py'), 'exec')
    scope = {'__name__': '__main__', '__file__': str(fake / 'tools/play/play_native.py')}
    with mock.patch.dict(sys.modules, {'localization_launch': stub, 'native_binary_launch': binary_selector,
                                      'performance_mode': performance}), \
         mock.patch.dict(os.environ, base_env, clear=True), \
         mock.patch.object(subprocess, 'Popen', popen), \
         mock.patch.object(sys, 'path', [str(ROOT / 'tools/play'), *sys.path]), \
         mock.patch.object(sys, 'argv', ['play_native.py']), \
         contextlib.redirect_stdout(io.StringIO()):
        try:
            exec(code, scope)
        except SystemExit as stop:
            if stop.code not in (0, None):
                raise RuntimeError(f'play_native.py exited with {stop.code!r}')
        finally:
            if 'lease' in scope:
                scope['lease'].close()
    if not captured:
        raise RuntimeError('play_native.py did not launch the game')
    return captured['env'], captured['command'], captured['cwd'], fake


def to_logical(value, root):
    """Map absolute paths under root to <name> tokens using PATH_MAP keys or values."""
    root = str(root).rstrip('\\/')
    if not value.lower().startswith(root.lower() + '\\'):
        return value
    rel = value[len(root) + 1:].replace('\\', '/')
    for workspace, dist in PATH_MAP.items():
        if rel.lower() in (workspace.lower(), dist.lower()):
            return '<' + dist + '>'
    return '<UNMAPPED ' + rel + '>'


def build_stub(directory):
    exe = directory / 'stub.exe'
    command = ("Add-Type -TypeDefinition ([IO.File]::ReadAllText('%s')) -OutputAssembly '%s' -OutputType ConsoleApplication"
               % (directory / 'stub.cs', exe))
    (directory / 'stub.cs').write_text(STUB_SOURCE, encoding='utf-8')
    subprocess.run([str(POWERSHELL), '-NoProfile', '-NonInteractive', '-Command', command], check=True,
                   capture_output=True, timeout=120)
    return exe


def make_fixture(dist, stub, settings_text, base):
    """Minimal copy of the distribution with the stub in place of the game."""
    fixture = base / 'Macross30 test & (dist)'  # spaces, & and parentheses on purpose
    if fixture.exists():
        shutil.rmtree(fixture)
    (fixture / 'bin').mkdir(parents=True)
    (fixture / 'tools').mkdir()
    (fixture / 'korean').mkdir()
    (fixture / 'data').mkdir()
    shutil.copy2(dist / 'Macross30.cmd', fixture / 'Macross30.cmd')
    shutil.copy2(dist / 'tools/launch-helper.ps1', fixture / 'tools/launch-helper.ps1')
    shutil.copy2(stub, fixture / 'bin/Macross30Recompiled.exe')
    (fixture / 'bin/EBOOT.ko.elf').write_bytes(b'')
    (fixture / 'korean/data-version.txt').write_text('test-version\r\n', encoding='utf-8')
    (fixture / 'data/installed-version.txt').write_text('test-version\r\n', encoding='utf-8')
    if settings_text is not None:
        (fixture / 'play').mkdir()
        (fixture / 'play/settings.ini').write_bytes(settings_text.encode('utf-8'))
    return fixture


def run_launcher(fixture, base_env):
    # Same command line Explorer uses for a double-click: cmd /c ""<script>"" (outer quotes survive & and parentheses).
    result = subprocess.run(f'cmd.exe /d /c ""{fixture / "Macross30.cmd"}""', env=base_env, cwd=str(fixture.parent),
                            stdin=subprocess.DEVNULL, capture_output=True, timeout=120)
    if result.returncode != 0:
        raise RuntimeError(f'launcher failed ({result.returncode}): {result.stdout!r} {result.stderr!r}')
    env, args, cwd = {}, [], None
    for line in (fixture / 'play/stdout.log').read_text(encoding='utf-8').splitlines():
        kind, _, rest = line.partition('\t')
        if kind == 'CWD':
            cwd = rest
        elif kind == 'ARG':
            args.append(rest)
        elif kind == 'ENV':
            key, _, value = rest.partition('\t')
            env[key] = value
    return env, args, cwd, result


def compare(py_env, py_cmd, py_cwd, py_root, cmd_env, cmd_args, cmd_cwd, cmd_root):
    left = {k.upper(): to_logical(v, py_root) for k, v in py_env.items()}
    right = {k.upper(): to_logical(v, cmd_root) for k, v in cmd_env.items()}
    problems = []
    for key in sorted(set(left) | set(right)):
        if left.get(key) != right.get(key):
            problems.append(f'{key}: play_native={left.get(key)!r} launcher={right.get(key)!r}')
    py_command = [to_logical(c, py_root) for c in py_cmd]
    cmd_command = [to_logical(c, cmd_root) for c in cmd_args]
    if py_command != cmd_command:
        problems.append(f'command: play_native={py_command} launcher={cmd_command}')
    if to_logical(py_cwd, py_root) != to_logical(cmd_cwd.rstrip('\\'), cmd_root):
        problems.append(f'cwd: play_native={py_cwd} launcher={cmd_cwd}')
    return problems


def flag_files():
    """cwd flag files the runtime polls (GetFileAttributesA / LD_FILE_FLAG) and which exist in native/play."""
    names = set()
    pattern = re.compile(r'(?:GetFileAttributesA|LD_FILE_FLAG)\(\s*"([^"/\\:%]+)"')
    for base in (ROOT / 'vendor/ps3recomp-master/libs', ROOT / 'vendor/ps3recomp-master/runtime', ROOT / 'native/macross30'):
        for path in base.rglob('*'):
            if path.suffix in ('.c', '.cpp', '.h') and 'tests' not in path.parts:
                names.update(pattern.findall(path.read_text(encoding='utf-8', errors='replace')))
    present = sorted(n for n in names if (ROOT / 'native/play' / n).exists())
    return sorted(names), present


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--dist', type=Path, default=Path(r'C:\GameDev\Macross-dist\Macross30'))
    parser.add_argument('--report', type=Path, help='write a JSON report here')
    args = parser.parse_args()
    dist = args.dist.resolve()
    base_env = {k: v for k, v in os.environ.items()}
    base_env.update(POLLUTION)
    base_env['macross_lower_case'] = '1'
    workspace_settings = ROOT / 'native/play/settings.ini'
    report = dict(dist=str(dist), cases={}, ok=True)
    profile_path = dist / 'release-profile.json'
    profile = json.loads(profile_path.read_text(encoding='utf-8')) if profile_path.exists() else {}
    policy = {'stable': STABLE_ENV, 'normalized': NORMALIZED_ENV}.get(profile.get('name'), {})
    assert profile.get('environment', {}) == policy, 'Release profile differs from reviewed policy'
    with tempfile.TemporaryDirectory(prefix='m30-envcheck-') as tmp:
        tmp = Path(tmp).resolve()
        stub = build_stub(tmp)
        for name, settings in SETTINGS_CASES.items():
            if settings == 'WORKSPACE':
                settings = workspace_settings.read_text(encoding='utf-8', errors='replace') if workspace_settings.exists() else None
            py_env, py_cmd, py_cwd, fake = play_native_launch(settings, base_env)
            py_env.update(policy)
            py_env['MACROSS_BATTLE_SUBTITLES'] = str(int(profile.get('binary_provenance', {}).get('battle_subtitles', False)))
            try:
                fixture = make_fixture(dist, stub, settings, tmp)
                cmd_env, cmd_args, cmd_cwd, _ = run_launcher(fixture, base_env)
                problems = compare(py_env, py_cmd, py_cwd, fake, cmd_env, cmd_args, cmd_cwd, fixture)
                # Second launch: previous logs must rotate into play/history and process.json must parse.
                run_launcher(fixture, base_env)
                history = list((fixture / 'play/history').glob('*/stdout.log'))
                process = json.loads((fixture / 'play/process.json').read_text(encoding='utf-8'))
                exit_info = json.loads((fixture / 'play/exit.json').read_text(encoding='utf-8'))
                if len(history) != 1:
                    problems.append(f'history rotation: expected 1 rotated run, found {len(history)}')
                if exit_info.get('exit_code') != 0 or len(process.get('command', [])) != 3:
                    problems.append(f'process/exit json unexpected: {process} {exit_info}')
            finally:
                shutil.rmtree(fake, ignore_errors=True)
            report['cases'][name] = dict(ok=not problems, problems=problems,
                                         render_scale=py_env.get('LD_RENDER_SCALE'),
                                         uncapped=py_env.get('MACROSS_REALTIME_STEP') == 'caller',
                                         game_variables=len([k for k in py_env if k not in base_env or py_env[k] != base_env.get(k)]))
            report['ok'] &= not problems
            print(f"{'OK  ' if not problems else 'FAIL'} {name}: LD_RENDER_SCALE={py_env.get('LD_RENDER_SCALE')} "
                  f"uncapped={report['cases'][name]['uncapped']}", flush=True)
            for problem in problems:
                print('     ', problem)
    names, present = flag_files()
    report['cwd_flag_files_known'] = names
    report['cwd_flag_files_in_workspace_play'] = present
    if present:
        print('NOTE: workspace native/play has runtime flag files the fresh distribution does not:', ', '.join(present))
    if args.report:
        args.report.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('Environment check', 'PASSED' if report['ok'] else 'FAILED')
    raise SystemExit(0 if report['ok'] else 1)


if __name__ == '__main__':
    main()
