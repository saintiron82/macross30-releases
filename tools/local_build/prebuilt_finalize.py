"""Verify a supplied runtime and locally create only the user's patched ELF files."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import tempfile

from elf_patch import apply
from language_views import child, ready, write_changed, prepare_views

MODE = 'user-original-eboot'
ORIGINAL_SHA256 = '44d7112cbb5527314eb6192e59d861dacfcd4099f301a6e82ffc1681b0782b5b'
DLLS = {'SDL2.dll', 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140_atomic_wait.dll',
        'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_codecvt_ids.dll', 'vcruntime140_threads.dll'}
FFMPEG = {'avutil-61.dll', 'avcodec-63.dll', 'avformat-63.dll', 'swresample-7.dll'}
BASE_FILES = {'CollectLogs.cmd', 'DistInstaller.cs', 'Macross30.cmd', 'launch-tools/collect-logs.ps1',
              'launch-tools/launch-helper.ps1', 'plans/ja-JP.tsv', 'recipe.json'}
TOOLS = {'install_language.ps1', 'setup_tools.ps1', 'original_elf.py', 'elf_patch.py', 'language_views.py', 'install_prebuilt.ps1', 'prebuilt_finalize.py', 'aircraft_hd.py'}


def sha(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def allowed(path):
    if path in {'bin/MacrossShaderPrep.exe', 'bin/assets/shader-seed-manifest-v3.json', 'bin/assets/rsx_pso_inventory_v1.bin', 'bin/assets/aircraft-hd-recipe.json'}:
        return True
    if re.fullmatch(r'bin/assets/rsx_shader_cache_v2/[VPB]_[0-9A-F]{16}_[0-9A-F]{8}\.bin', path):
        return True
    return (path in {'base-game/' + name for name in BASE_FILES} or
            path in {'bin/Macross30Recompiled.exe', 'THIRD-PARTY-NOTICES.txt', 'tools/play/inspect_iso.py'} or
            path in {'bin/' + name for name in DLLS} or path in {'bin/ffmpeg/' + name for name in FFMPEG} or
            path in {'tools/local_build/' + name for name in TOOLS})


def program_manifest(program):
    program = Path(program).resolve()
    manifest = json.loads((program / 'program-manifest.json').read_text(encoding='utf8'))
    if manifest.get('mode') != MODE or manifest.get('original_elf_sha256') != ORIGINAL_SHA256:
        raise ValueError('Unsupported prebuilt program manifest')
    names = set()
    for row in manifest['files']:
        name = row['path']
        if '\\' in name or ':' in name or PurePosixPath(name).is_absolute() or '..' in PurePosixPath(name).parts or not allowed(name):
            raise ValueError('Unlisted or unsafe program file: ' + name)
        if name.lower() in names:
            raise ValueError('Duplicate program file')
        names.add(name.lower())
    required = {'bin/Macross30Recompiled.exe', 'bin/SDL2.dll', 'bin/msvcp140.dll', 'bin/vcruntime140.dll', 'bin/vcruntime140_1.dll'}
    required |= {'bin/ffmpeg/' + name for name in FFMPEG}
    if not {name.lower() for name in required}.issubset(names):
        raise ValueError('Incomplete prebuilt runtime')
    return manifest


def verify_program(program):
    program = Path(program).resolve()
    manifest = program_manifest(program)
    for row in manifest['files']:
        path = child(program, row['path'])
        if row['path'].startswith('bin/') and path.suffix.lower() in ('.exe', '.dll') and path.is_file():
            with path.open('rb') as stream:
                if stream.read(2) != b'MZ':
                    raise ValueError('Non-PE runtime payload: ' + row['path'])
        if not path.is_file() or path.stat().st_size != row['bytes'] or sha(path) != row['sha256']:
            raise ValueError('Program file hash mismatch: ' + row['path'])
    return manifest


def install_program(root, program):
    root, program = Path(root).resolve(), Path(program).resolve()
    manifest = verify_program(program)
    for row in manifest['files']:
        if not row['path'].startswith('bin/') and row['path'] != 'THIRD-PARTY-NOTICES.txt':
            continue
        source = child(program, row['path'])
        target = child(root, row['path'])
        if target.is_file() and target.stat().st_size == row['bytes'] and sha(target) == row['sha256']:
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        pending = target.with_name(target.name + '.pending')
        shutil.copyfile(source, pending)
        if sha(pending) != row['sha256']:
            raise ValueError('Runtime copy verification failed')
        os.replace(pending, target)
    return manifest


def prepare_elf(root, payload, original_elf, locale=None):
    root, payload = Path(root).resolve(), Path(payload).resolve()
    recipe = json.loads((payload / 'recipe.json').read_text(encoding='utf8'))
    original = Path(original_elf).read_bytes()
    if recipe['original_elf_sha256'] != ORIGINAL_SHA256 or hashlib.sha256(original).hexdigest() != ORIGINAL_SHA256:
        raise ValueError('Supported user original ELF required')
    if locale is not None and locale not in recipe['languages']:
        raise ValueError('Selected language is not included in this payload')
    result = {}
    for locale in ([locale] if locale else ('ko-KR', 'ja-JP', 'en-US')):
        if locale not in recipe['languages']:
            continue
        language = recipe['languages'][locale]
        localized = apply(original, child(payload, language['elf_patch']).read_bytes()) if 'elf_patch' in language else original
        if hashlib.sha256(localized).hexdigest() != language['elf_sha256']:
            raise ValueError('Locally patched ELF mismatch: ' + locale)
        name = 'bin/EBOOT.' + locale + '.elf'
        write_changed(child(root, name), localized)
        result[locale] = dict(path=name, sha256=language['elf_sha256'])
    return recipe, result


def launcher_command(template):
    setup = r'''set "_M30_LANG=ko-KR"
if exist "%_M30_ROOT%\.local-build\game-language.txt" set /p _M30_LANG=<"%_M30_ROOT%\.local-build\game-language.txt"
if not "%_M30_LANG%"=="ko-KR" if not "%_M30_LANG%"=="ja-JP" if not "%_M30_LANG%"=="en-US" set "_M30_LANG=ko-KR"
set "_M30_UI=ko"
if exist "%_M30_ROOT%\.local-build\ui-language.txt" set /p _M30_UI=<"%_M30_ROOT%\.local-build\ui-language.txt"
if not "%_M30_UI%"=="ko" if not "%_M30_UI%"=="en" if not "%_M30_UI%"=="ja" set "_M30_UI=ko"
'''
    index = template.index('set "_M30_EXE=')
    command = template[:index] + setup + template[index:]
    command = command.replace('bin\\EBOOT.local.elf', 'bin\\EBOOT.%_M30_LANG%.elf')
    command = command.replace('data\\disc', 'data\\languages\\%_M30_LANG%\\data\\disc')
    command = command.replace('.local-build\\data-version.txt', 'data\\languages\\%_M30_LANG%\\expected-version.txt')
    command = command.replace('data\\installed-version.txt', 'data\\languages\\%_M30_LANG%\\data\\installed-version.txt')
    command = command.replace('bin\\subtitles', 'bin\\subtitles\\%_M30_LANG%')
    command = command.replace('set "MACROSS_BATTLE_SUBTITLES=1"', 'set "MACROSS_BATTLE_SUBTITLES=1"\nif "%_M30_LANG%"=="ja-JP" set "MACROSS_BATTLE_SUBTITLES=0"')
    command = command.replace('set "PS3_HDD0_ROOT=%_M30_ROOT%\\data\\hdd0"',
                              'set "PS3_HDD0_ROOT=%_M30_ROOT%\\data\\hdd0"\n'
                              'set "PS3_HDD0_GAME_ROOT=%_M30_ROOT%\\data\\languages\\%_M30_LANG%\\data\\hdd0\\game"\n'
                              'set "MACROSS_GAME_LANGUAGE=%_M30_LANG%"\nset "MACROSS_UI_LANGUAGE=%_M30_UI%"\n'
                              'if "%_M30_LANG%"=="en-US" set "MACROSS_BATTLE_CATALOG_FILE=%_M30_ROOT%\\bin\\subtitles\\en-US\\battle.en.tsv"\n'
                              'set "RSX_SHADER_CACHE_DIR=%LOCALAPPDATA%\\Macross30\\shader-cache-v2"\n'
                              'if not exist "%LOCALAPPDATA%\\Macross30\\shader-cache-v2" mkdir "%LOCALAPPDATA%\\Macross30\\shader-cache-v2"')
    return command.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf8')


def migrate_shader_cache(root, cache_root=None):
    """Retain only this installation's valid v2 cache; never overwrite or delete."""
    root = Path(root).resolve()
    original = child(root, 'play/scratch/rsx_shader_cache_v2')
    if not original.is_dir():
        return 0
    if cache_root is None:
        local = os.environ.get('LOCALAPPDATA')
        if not local:
            return 0
        cache_root = Path(local) / 'Macross30/shader-cache-v2'
    cache_root = Path(cache_root)
    cache_root.mkdir(parents=True, exist_ok=True)
    copied = 0
    for file in original.iterdir():
        match = re.fullmatch(r'([VP])_([0-9A-Fa-f]{16})_([0-9A-Fa-f]{8})\.bin', file.name)
        if not match or not file.is_file() or file.is_symlink() or not 32 <= file.stat().st_size <= 4 * 1024 * 1024:
            continue
        data = file.read_bytes()
        magic, version, stage, source_length, blob_length, reserved, source_hash = struct.unpack_from('<6IQ', data)
        if (magic != 0x43535A59 or version != 2 or stage != ord(match[1]) or source_hash != int(match[2], 16) or
                source_length != int(match[3], 16) or source_length > 256 * 1024 or blob_length == 0 or
                len(data) != 32 + source_length + blob_length):
            continue
        destination = cache_root / file.name
        if destination.exists():
            continue
        with tempfile.NamedTemporaryFile(prefix='migrate-', suffix='.tmp', dir=cache_root, delete=False) as stream:
            stream.write(data)
            temporary = Path(stream.name)
        try:
            try:
                os.link(temporary, destination)
            except FileExistsError:
                continue
            except OSError:
                # On Windows rename is atomic and refuses an existing target.
                # On other systems without hardlinks, leave the cache untouched.
                if os.name != 'nt':
                    continue
                try:
                    os.rename(temporary, destination)
                except FileExistsError:
                    continue
            copied += 1
        finally:
            temporary.unlink(missing_ok=True)
    print('BOOT_SHADER_CACHE_MIGRATED: ' + str(copied), flush=True)
    return copied


def finalize(root, program, payload, original_elf, locale, release_version=None):
    root, payload = Path(root).resolve(), Path(payload).resolve()
    manifest = program_manifest(program)
    recipe, elves = prepare_elf(root, payload, original_elf, locale)
    views = json.loads((root / '.local-build/language-views.json').read_text(encoding='utf8'))['languages']
    if locale not in views or locale not in elves or not ready(root, views[locale]):
        raise ValueError('Selected language must be prepared before completion')
    # An unrelated broken or not-yet-installed language must not block this one.
    views = {code: row for code, row in views.items() if ready(root, row) and child(root, row['elf']).is_file()}
    runtime = [row for row in manifest['files'] if row['path'].startswith('bin/')]
    for row in runtime:
        path = child(root, row['path'])
        if not path.is_file() or sha(path) != row['sha256']:
            raise ValueError('Installed runtime verification failed: ' + row['path'])
    for language in (locale,):
        destination = root / 'bin/subtitles' / language
        destination.mkdir(parents=True, exist_ok=True)
        subtitle_source = payload / ('subtitles-en' if language == 'en-US' else 'subtitles')
        if language in ('ko-KR', 'en-US'):
            for file in subtitle_source.rglob('*'):
                if file.is_file():
                    write_changed(child(destination, file.relative_to(subtitle_source).as_posix()), file.read_bytes())
    write_changed(root / 'bin/EBOOT.local.elf', (root / elves[locale]['path']).read_bytes())
    write_changed(root / 'Macross30.cmd', launcher_command((payload / 'Macross30.cmd').read_text(encoding='utf8')))
    for file in (payload / 'launch-tools').rglob('*'):
        if file.is_file():
            write_changed(child(root / 'tools', file.relative_to(payload / 'launch-tools').as_posix()), file.read_bytes())
    write_changed(root / 'CollectLogs.cmd', (payload / 'CollectLogs.cmd').read_bytes())
    migrate_shader_cache(root)
    state = root / '.local-build'
    write_changed(state / 'data-version.txt', (views[locale]['data_id'] + '\n').encode())
    write_changed(state / 'game-language.txt', (locale + '\n').encode())
    for file, value in [('ui-language.txt', 'ko')]:
        if not (state / file).exists():
            write_changed(state / file, (value + '\n').encode())
    exe = next(row for row in runtime if row['path'] == 'bin/Macross30Recompiled.exe')
    profile = dict(release_version=release_version or manifest['version'], mode=MODE,
                   game_language=locale, supported_languages=list(views), language_views=views,
                   original_elf_sha256=ORIGINAL_SHA256, elf_sha256=elves[locale]['sha256'], exe_sha256=exe['sha256'],
                   generated_on_this_machine=False, downloaded_game_executable=True, original_elf_prepared_locally=True, original_generated_locally=True,
                   program_files=runtime)
    write_changed(root / 'release-profile.json', (json.dumps(profile, indent=2) + '\n').encode())
    if (root / 'bin/assets/aircraft-hd-recipe.json').is_file():
        from aircraft_hd import prepare
        print('HD_TEXTURES_PREPARING: rebuilding from local original data', flush=True)
        print('HD_TEXTURES_READY: '+json.dumps(prepare(root)), flush=True)
    print('BOOT_INSTALL_COMPLETE: prebuilt runtime verified; only user ELF/data prepared locally', flush=True)
    return profile


def install_language(root, payload, original_elf, locale, iso='.', runner=None):
    """Install translation only; the common runtime and original view are immutable."""
    if locale not in ('ko-KR', 'en-US'):
        raise ValueError('Only translated languages have a language-pack installer')
    root, payload, original_elf = Path(root).resolve(), Path(payload).resolve(), Path(original_elf).resolve()
    print('BOOT_STAGE 2/7 Checking prepared original game', flush=True)
    profile_path = root / 'release-profile.json'
    profile = json.loads(profile_path.read_text(encoding='utf8'))
    state_path = root / '.local-build/language-views.json'
    state = json.loads(state_path.read_text(encoding='utf8'))
    base = state.get('languages', {}).get('ja-JP')
    if (profile.get('mode') != MODE or not profile.get('original_generated_locally') or
            not base or not ready(root, base) or sha(original_elf) != ORIGINAL_SHA256 or
            sha(child(root, base['elf'])) != ORIGINAL_SHA256):
        raise ValueError('Prepare the original game before installing a language pack')
    print('BOOT_STAGE 3/7 Preparing selected translated ELF', flush=True)
    prepare_elf(root, payload, original_elf, locale)
    print('BOOT_STAGE 4/7 Installing selected translation data', flush=True)
    kwargs = {'locale': locale}
    if runner is not None:
        kwargs['runner'] = runner
    state = prepare_views(root, payload, iso, **kwargs)
    print('BOOT_STAGE 5/7 Installing selected subtitles', flush=True)
    source = payload / ('subtitles-en' if locale == 'en-US' else 'subtitles')
    for file in source.rglob('*'):
        if file.is_file():
            write_changed(child(root / 'bin/subtitles' / locale, file.relative_to(source).as_posix()), file.read_bytes())
    print('BOOT_STAGE 6/7 Recording installed language', flush=True)
    row = state['languages'][locale]
    if not ready(root, row) or sha(child(root, row['elf'])) != row['elf_sha256']:
        raise ValueError('Selected language verification failed')
    # Keep core release/runtime identity and selected language unchanged.
    profile.setdefault('language_views', {})[locale] = row
    profile['supported_languages'] = list(profile['language_views'])
    write_changed(profile_path, (json.dumps(profile, indent=2) + '\n').encode())
    print('BOOT_STAGE 7/7 Selected language verified', flush=True)
    print('BOOT_LANGUAGE_COMPLETE: common game unchanged', flush=True)
    return profile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--action', choices=('program', 'elf', 'finalize', 'language'), required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--program', type=Path)
    parser.add_argument('--payload', type=Path)
    parser.add_argument('--original-elf', type=Path)
    parser.add_argument('--locale', choices=('ko-KR', 'ja-JP', 'en-US'))
    parser.add_argument('--release-version')
    parser.add_argument('--iso', default='.')
    args = parser.parse_args()
    if args.action == 'program':
        install_program(args.root, args.program)
    elif args.action == 'language':
        install_language(args.root, args.payload, args.original_elf, args.locale, args.iso)
    elif args.action == 'elf':
        prepare_elf(args.root, args.payload, args.original_elf, args.locale)
    else:
        finalize(args.root, args.program, args.payload, args.original_elf, args.locale or 'ko-KR', args.release_version)


if __name__ == '__main__':
    main()
