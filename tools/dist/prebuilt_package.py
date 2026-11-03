"""Package a supplied Windows build; users locally prepare only original-derived ELF/data.

No compiler is run by this module. The caller must explicitly select program-dir.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import sys
import zipfile

from split_language_packages import split, base_payload

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/shader_cache'))
import stage_program_assets  # noqa: E402  shader-prep opt-in contract
MODE = 'user-original-eboot'
ORIGINAL_SHA256 = '44d7112cbb5527314eb6192e59d861dacfcd4099f301a6e82ffc1681b0782b5b'
REQUIRED_DLLS = ('SDL2.dll', 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
OPTIONAL_DLLS = ('msvcp140_atomic_wait.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_codecvt_ids.dll', 'vcruntime140_threads.dll')
FFMPEG = ('avutil-61.dll', 'avcodec-63.dll', 'avformat-63.dll', 'swresample-7.dll')
# First launcher whose PrebuiltInstall.Allowed accepts shader-prep assets and that runs
# graphics_prep before launch; older launchers reject such program packages.
SHADER_PREP_MIN_LAUNCHER = '0.3.8'
INSTALL_TOOLS = ('install_language.ps1', 'setup_tools.ps1', 'original_elf.py', 'elf_patch.py', 'language_views.py', 'install_prebuilt.ps1', 'prebuilt_finalize.py', 'aircraft_hd.py')


def sha(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def validate_data(path):
    with zipfile.ZipFile(path) as archive:
        for entry in archive.infolist():
            name = entry.filename
            if not name.startswith('payload/') or '..' in Path(name).parts or '\\' in name or ':' in name or entry.is_dir():
                raise ValueError('Invalid payload path: ' + name)
            if Path(name).suffix.lower() in {'.exe', '.elf', '.self', '.bin', '.iso', '.prx', '.sprx'}:
                raise ValueError('Original/full executable payload prohibited: ' + name)
            with archive.open(entry) as stream:
                if stream.read(4) in (b'\x7fELF', b'SCE\0', b'MZ\x90\0'):
                    raise ValueError('Executable disguised in language payload: ' + name)


def data_languages(path):
    with zipfile.ZipFile(path) as archive:
        recipe = json.loads(archive.read('payload/recipe.json'))
    return [locale for locale in ('ko-KR', 'ja-JP', 'en-US') if locale in recipe['languages']]


def package(program_dir, runtime_dir, data_archive, output, version='0.3.1', require_avx2=False,
            shader_seed=None, shader_prep_tool=None, hd_recipe=None):
    program_dir, runtime_dir, data_archive, output = map(Path, (program_dir, runtime_dir, data_archive, output))
    exe = program_dir / 'Macross30Recompiled.exe'
    if not exe.is_file():
        raise ValueError('Explicit program-dir must contain Macross30Recompiled.exe')
    with exe.open('rb') as stream:
        if stream.read(2) != b'MZ':
            raise ValueError('Selected game executable is not a Windows PE file')
    validate_data(data_archive)
    files = {'bin/Macross30Recompiled.exe': exe}
    def find(name, required=True):
        for directory in (program_dir, runtime_dir):
            candidate = directory / name
            if candidate.is_file():
                return candidate
        if required:
            raise ValueError('Required runtime file missing: ' + name)
    for name in REQUIRED_DLLS:
        files['bin/' + name] = find(name)
    for name in OPTIONAL_DLLS:
        candidate = find(name, False)
        if candidate:
            files['bin/' + name] = candidate
    for name in FFMPEG:
        files['bin/ffmpeg/' + name] = find('ffmpeg/' + name)
    # Optional shader seed + portable PSO inventory + local preparation tool;
    # both inputs or neither, verified, never a GPU/driver pipeline library.
    shader_files, shader_prep = stage_program_assets.program_files(shader_seed, shader_prep_tool, exe)
    files.update(shader_files)
    if hd_recipe:
        spec = json.loads(Path(hd_recipe).read_text(encoding='utf-8'))
        if spec.get('schema') != 1 or spec.get('count') != 245 or len(spec.get('entries', [])) != 245:
            raise ValueError('Invalid pinned HD recipe')
        files['bin/assets/aircraft-hd-recipe.json'] = Path(hd_recipe)
    files['THIRD-PARTY-NOTICES.txt'] = ROOT / 'tools/dist/template/THIRD-PARTY-NOTICES.txt'
    for name in INSTALL_TOOLS:
        files['tools/local_build/' + name] = ROOT / 'tools/local_build' / name
    files['tools/play/inspect_iso.py'] = ROOT / 'tools/play/inspect_iso.py'
    output.mkdir(parents=True, exist_ok=True)
    (output / 'BOOT-INSTALL.json').unlink(missing_ok=True)
    archive_path = output / ('Macross30-program-' + version + '.zip')
    requirements = {'avx2': False, 'sse41': False}
    cache = program_dir / 'CMakeCache.txt'
    if cache.is_file():
        text = cache.read_text(encoding='utf8')
        requirements['avx2'] = bool(re.search(r'^MACROSS_GAMEPLAY_AVX2:BOOL=ON$|^MACROSS_POSTTASK_ISA:STRING=avx2$', text, re.M))
        requirements['sse41'] = requirements['avx2'] or bool(re.search(r'^MACROSS_SPU_SSE41:BOOL=ON$', text, re.M))
    if require_avx2:
        requirements['avx2'] = requirements['sse41'] = True
    base_files, base_language = base_payload(data_archive, version)
    rows = []
    with zipfile.ZipFile(archive_path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for relative, source in sorted(files.items()):
            if relative == 'tools/local_build/original_elf.py':
                # Export only the independently implemented ISO/SELF preparation tool.
                # Its one constant must not pull in the old code-generation pipeline.
                text = source.read_text(encoding='utf8')
                text = text.replace('from prepare import ORIGINAL_SHA256', "ORIGINAL_SHA256 = '" + ORIGINAL_SHA256 + "'")
                data = text.encode('utf8')
                archive.writestr('program/' + relative, data)
                rows.append(dict(path=relative, bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
            else:
                archive.write(source, 'program/' + relative)
                rows.append(dict(path=relative, bytes=source.stat().st_size, sha256=sha(source)))
        for name, data in sorted(base_files.items()):
            relative = 'base-game/' + name
            archive.writestr('program/' + relative, data)
            rows.append(dict(path=relative, bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
        program_manifest = dict(mode=MODE, version=version, original_elf_sha256=ORIGINAL_SHA256,
                                runtime_requirements=requirements, files=rows)
        if shader_prep:
            program_manifest['shader_prep'] = shader_prep
        archive.writestr('program/program-manifest.json', json.dumps(program_manifest, indent=2))
    language_packs = split(data_archive, output, version)
    # Remove the obsolete artifact from drafts created before original-game
    # preparation became part of the common program package.
    (output / ('Macross30-language-ja-JP-' + version + '.zip')).unlink(missing_ok=True)
    (output / 'LANGUAGE-PACKS.json').write_text(json.dumps(language_packs, indent=2) + '\n', encoding='utf8')
    manifest = dict(version=version, mode=MODE, min_launcher_version=SHADER_PREP_MIN_LAUNCHER if shader_prep else '0.2.4', source_only=False,
                    original_elf_sha256=ORIGINAL_SHA256, languages=data_languages(data_archive), runtime_requirements=requirements,
                    files={'program': dict(file=archive_path.name, bytes=archive_path.stat().st_size, sha256=sha(archive_path), exe_sha256=sha(exe))},
                    language_packs=language_packs, base_language=base_language)
    (output / 'BOOT-INSTALL.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf8')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--program-dir', required=True, type=Path)
    parser.add_argument('--runtime-dir', required=True, type=Path)
    parser.add_argument('--data-archive', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--version', default='0.3.1')
    parser.add_argument('--require-avx2', action='store_true', help='For supplied candidate executables that inherit AVX2 but have no CMakeCache.txt')
    parser.add_argument('--shader-seed', type=Path, help='tools/shader_cache/shader_seed.py build output (opt-in, needs --shader-prep-tool)')
    parser.add_argument('--shader-prep-tool', type=Path, help='MacrossShaderPrep.exe built from native/macross30/shader_prep_main.c')
    parser.add_argument('--hd-recipe', type=Path)
    args = parser.parse_args()
    print(json.dumps(package(args.program_dir, args.runtime_dir, args.data_archive, args.output, args.version, args.require_avx2,
                             args.shader_seed, args.shader_prep_tool, args.hd_recipe), indent=2))


if __name__ == '__main__':
    main()

