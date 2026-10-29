"""Install only locally generated game executables and user-derived ELF data."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

from elf_patch import apply

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--payload', required=True, type=Path)
    parser.add_argument('--runtime', required=True, type=Path)
    parser.add_argument('--original-elf', required=True, type=Path)
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
    language = recipe['languages'][args.locale]
    localized = apply(original, (args.payload / language['elf_patch']).read_bytes()) if 'elf_patch' in language else original
    if hashlib.sha256(localized).hexdigest() != language['elf_sha256']:
        raise ValueError('User-local generated ELF mismatch')
    (target / 'bin/EBOOT.local.elf').write_bytes(localized)
    subtitle_source = args.payload / 'subtitles'
    if args.locale == 'ko-KR':
        shutil.copytree(subtitle_source, target / 'bin/subtitles', dirs_exist_ok=True)
    else:
        (target / 'bin/subtitles-ja').mkdir(parents=True, exist_ok=True)
    command = (args.payload / 'Macross30.cmd').read_text(encoding='utf8')
    if args.locale == 'ja-JP':
        command = command.replace('MACROSS_BATTLE_SUBTITLES=1', 'MACROSS_BATTLE_SUBTITLES=0').replace('bin\\subtitles', 'bin\\subtitles-ja')
    (target / 'Macross30.cmd').write_bytes(command.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf8'))
    shutil.copytree(args.payload / 'launch-tools', target / 'tools', dirs_exist_ok=True)
    shutil.copyfile(args.payload / 'CollectLogs.cmd', target / 'CollectLogs.cmd')
    state = target / '.local-build'
    state.mkdir(exist_ok=True)
    (state / 'data-version.txt').write_text(language['data_id'] + '\n', encoding='ascii')
    profile = dict(release_version=recipe['version'], mode='user-local-generate-and-compile',
                   game_language=args.locale, original_elf_sha256=recipe['original_elf_sha256'],
                   elf_sha256=language['elf_sha256'], exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                   generated_on_this_machine=True, downloaded_game_executable=False)
    (target / 'release-profile.json').write_text(json.dumps(profile, indent=2) + '\n', encoding='utf8')
    print('LOCAL_GENERATION_INSTALLED: user-derived ELF and locally compiled EXE', flush=True)


if __name__ == '__main__':
    main()
