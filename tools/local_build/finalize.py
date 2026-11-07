"""Install only locally generated game executables and user-derived ELF data."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

from elf_patch import apply
from language_views import ready, write_changed

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--payload', required=True, type=Path)
    parser.add_argument('--runtime', required=True, type=Path)
    parser.add_argument('--original-elf', required=True, type=Path)
    parser.add_argument('--release-version')
    parser.add_argument('--locale', required=True, choices=('ko-KR', 'ja-JP'))
    args = parser.parse_args()
    recipe = json.loads((args.payload / 'recipe.json').read_text(encoding='utf8'))
    original = args.original_elf.read_bytes()
    if hashlib.sha256(original).hexdigest() != recipe['original_elf_sha256']:
        raise ValueError('User original ELF changed; no downloaded fallback')
    target = args.root.resolve()
    (target / 'bin').mkdir(parents=True, exist_ok=True)
    exe = ROOT / 'native/build/Macross30Recompiled.exe'
    if not exe.is_file():
        raise ValueError('User-local native build is not complete')
    shutil.copyfile(exe, target / 'bin/Macross30Recompiled.exe')
    shutil.copyfile(ROOT / 'native/build/SDL2.dll', target / 'bin/SDL2.dll')
    for file in args.runtime.rglob('*'):
        if file.is_file():
            out = target / file.relative_to(args.runtime)
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(file, out)
    views = json.loads((target / '.local-build/language-views.json').read_text(encoding='utf8'))['languages']
    if args.locale not in views or not all(ready(target, row) for row in views.values()):
        raise ValueError('Every selectable language must be prepared before finalization')
    for locale, info in views.items():
        language = recipe['languages'][locale]
        localized = apply(original, (args.payload / language['elf_patch']).read_bytes()) if 'elf_patch' in language else original
        if hashlib.sha256(localized).hexdigest() != language['elf_sha256']:
            raise ValueError('User-local generated ELF mismatch')
        write_changed(target / info['elf'], localized)
        subtitle_target = target / 'bin/subtitles' / locale
        subtitle_target.mkdir(parents=True, exist_ok=True)
        if locale == 'ko-KR':
            for source in (args.payload / 'subtitles').rglob('*'):
                if source.is_file():
                    write_changed(subtitle_target / source.relative_to(args.payload / 'subtitles'), source.read_bytes())
    # Compatibility alias is created once at Apply. Language selection never rewrites it.
    selected = views[args.locale]
    write_changed(target / 'bin/EBOOT.local.elf', (target / selected['elf']).read_bytes())
    command = (args.payload / 'Macross30.cmd').read_text(encoding='utf8')
    language_setup = r'''set "_M30_LANG=ko-KR"
if exist "%_M30_ROOT%\.local-build\game-language.txt" set /p _M30_LANG=<"%_M30_ROOT%\.local-build\game-language.txt"
if not "%_M30_LANG%"=="ko-KR" if not "%_M30_LANG%"=="ja-JP" set "_M30_LANG=ko-KR"
set "_M30_UI=ko"
if exist "%_M30_ROOT%\.local-build\ui-language.txt" set /p _M30_UI=<"%_M30_ROOT%\.local-build\ui-language.txt"
if not "%_M30_UI%"=="ko" if not "%_M30_UI%"=="en" if not "%_M30_UI%"=="ja" set "_M30_UI=ko"
'''
    needle = 'set "_M30_EXE='
    position = command.index(needle)
    command = command[:position] + language_setup + command[position:]
    command = command.replace('bin\\EBOOT.local.elf', 'bin\\EBOOT.%_M30_LANG%.elf')
    command = command.replace('data\\disc', 'data\\languages\\%_M30_LANG%\\data\\disc')
    command = command.replace('.local-build\\data-version.txt', 'data\\languages\\%_M30_LANG%\\expected-version.txt')
    command = command.replace('data\\installed-version.txt', 'data\\languages\\%_M30_LANG%\\data\\installed-version.txt')
    command = command.replace('bin\\subtitles', 'bin\\subtitles\\%_M30_LANG%')
    command = command.replace('set "MACROSS_BATTLE_SUBTITLES=1"', 'set "MACROSS_BATTLE_SUBTITLES=1"\nif "%_M30_LANG%"=="ja-JP" set "MACROSS_BATTLE_SUBTITLES=0"')
    command = command.replace('set "PS3_HDD0_ROOT=%_M30_ROOT%\\data\\hdd0"',
                              'set "PS3_HDD0_ROOT=%_M30_ROOT%\\data\\hdd0"\n'
                              'set "PS3_HDD0_GAME_ROOT=%_M30_ROOT%\\data\\languages\\%_M30_LANG%\\data\\hdd0\\game"\n'
                              'set "MACROSS_GAME_LANGUAGE=%_M30_LANG%"\nset "MACROSS_UI_LANGUAGE=%_M30_UI%"')
    write_changed(target / 'Macross30.cmd', command.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf8'))
    shutil.copytree(args.payload / 'launch-tools', target / 'tools', dirs_exist_ok=True)
    shutil.copyfile(args.payload / 'CollectLogs.cmd', target / 'CollectLogs.cmd')
    state = target / '.local-build'
    state.mkdir(exist_ok=True)
    write_changed(state / 'data-version.txt', (selected['data_id'] + '\n').encode())
    if not (state / 'game-language.txt').exists():
        write_changed(state / 'game-language.txt', (args.locale + '\n').encode())
    if not (state / 'ui-language.txt').exists():
        write_changed(state / 'ui-language.txt', b'ko\n')
    profile = dict(release_version=args.release_version or recipe['version'], mode='user-local-generate-and-compile',
                   game_language=args.locale, supported_languages=list(views), language_views=views, original_elf_sha256=recipe['original_elf_sha256'],
                   elf_sha256=selected['elf_sha256'], exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                   generated_on_this_machine=True, downloaded_game_executable=False)
    (target / 'release-profile.json').write_text(json.dumps(profile, indent=2) + '\n', encoding='utf8')
    print('LOCAL_GENERATION_INSTALLED: user-derived ELF and locally compiled EXE', flush=True)


if __name__ == '__main__':
    main()
