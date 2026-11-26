"""Check actual native fallback/external catalogs and measured F9 label widths."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/tests/native-ui040-20261126'
CATALOGS = ROOT / 'localization/native-ui/0.4.0'
VIDEO = ROOT / 'vendor/ps3recomp-master/libs/video'
TOKEN = re.compile(r'%%|%[-+ #0]*\d*(?:\.\d+)?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcspn]')


def catalog(locale):
    lines = (CATALOGS / (locale + '.tsv')).read_text('utf8').splitlines()
    assert lines[0] == 'MACROSS_UI_STRINGS_V1\t' + locale
    return {k: v.replace('\\n','\n').replace('\\t','\t').replace('\\\\','\\')
            for k, v in (line.split('\t', 1) for line in lines[1:])}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    vs = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
                                  'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    compiler = Path(vs) / 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
    exe = OUT / 'native-ui.exe'
    command = [str(compiler), '/nologo', '/std:c17', '/O1', '/utf-8', '/I' + str(VIDEO),
               str(ROOT / 'tools/tests/native_ui040_fixture.c'), '/Fe:' + str(exe),
               '/link', 'gdi32.lib', 'user32.lib']
    with (OUT / 'ui-compile.log').open('w', encoding='utf8') as log:
        subprocess.run(command, cwd=OUT, stdout=log, stderr=subprocess.STDOUT, check=True)
    widths = {'options.fps_limit':200, 'options.fps_limit.unlimited':175,
              'options.rumble':382, 'options.rumble_strength':155, 'options.uncap':382,
              'mouse.enable':212, 'mouse.invert':162, 'mouse.sensitivity_x':195,
              'mouse.sensitivity_y':195, 'mouse.numeric_help':420, 'mouse.capture_help':420}
    english = catalog('en-US')
    results, problems = [], []
    for locale in ['ko-KR', 'en-US', 'ja-JP', 'ru-RU', 'es-ES', 'en-US-fix']:
        expected = catalog(locale)
        for external in [False, True]:
            env = dict(os.environ, MACROSS_GAME_LANGUAGE=locale)
            if external:
                env['MACROSS_UI_CATALOG_FILE'] = str(CATALOGS / (locale + '.tsv'))
            else:
                env.pop('MACROSS_UI_CATALOG_FILE', None)
            run = subprocess.run([str(exe)], cwd=OUT, env=env, capture_output=True, text=True, timeout=15)
            assert run.returncode == 0, (locale, external, run.stderr)
            seen = set()
            for line in run.stdout.splitlines():
                key, width, height, raw = line.split('\t')
                value = bytes.fromhex(raw).decode('utf8')
                assert key not in seen and value == expected[key], (locale, external, key, value)
                seen.add(key)
                assert TOKEN.findall(value) == TOKEN.findall(english[key]), (locale, key, 'format token mismatch')
                if key in widths and int(width) > widths[key]:
                    problems.append(dict(locale=locale, external=external, key=key, pixels=int(width), available=widths[key]))
                if key == 'options.restart_help' and int(height) > 80:
                    problems.append(dict(locale=locale, external=external, key=key, height=int(height), available=80))
            assert seen == set(expected), (locale, len(seen), len(expected))
            results.append(dict(locale=locale, external=external, keys=len(seen)))
    report = dict(passed=not problems, cases=results, resolved_strings=sum(r['keys'] for r in results),
                  text_width_problems=problems, game_started=False, visible_windows_created=False,
                  visual_check='actual game UI not opened; catalog and GDI metrics only')
    (OUT / 'UI-RESULT.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
    print(json.dumps(report, ensure_ascii=False, indent=2))
    assert not problems, 'F9 labels exceed measured control widths'


if __name__ == '__main__':
    main()
