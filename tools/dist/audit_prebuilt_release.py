"""Verify release hashes and the absence of complete PS3 executables/disc images."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'shader_cache'))
import stage_program_assets  # noqa: E402


def digest(data):
    return hashlib.sha256(data).hexdigest()


def audit(folder):
    folder = Path(folder)
    manifest = json.loads((folder / 'BOOT-INSTALL.json').read_text(encoding='utf-8'))
    if manifest['mode'] != 'user-original-eboot':
        raise ValueError('Unexpected installation mode')
    files = []
    shader_prep = None
    artifacts = list(manifest['files'].items())
    artifacts += [('language', row) for row in manifest.get('language_packs', {}).values()]
    for kind, row in artifacts:
        path = folder / row['file']
        if path.name != row['file']:
            raise ValueError('Unsafe artifact name')
        raw = path.read_bytes()
        if len(raw) != row['bytes'] or digest(raw) != row['sha256']:
            raise ValueError('Artifact identity mismatch: ' + path.name)
        with zipfile.ZipFile(path) as archive:
            names = set()
            for entry in archive.infolist():
                name = entry.filename
                parts = PurePosixPath(name).parts
                if name.lower() in names or '..' in parts or '\\' in name or ':' in name or name.startswith('/'):
                    raise ValueError('Unsafe or duplicate member: ' + name)
                names.add(name.lower())
                if entry.is_dir():
                    continue
                suffix = PurePosixPath(name).suffix.lower()
                if stage_program_assets.is_local_only(name):
                    raise ValueError('Local GPU/driver artifact in package: ' + name)
                shader_asset = kind == 'program' and name.startswith('program/') and stage_program_assets.is_allowed_bin(name[len('program/'):])
                if not shader_asset and suffix in {'.elf', '.self', '.iso', '.prx', '.sprx', '.obj', '.o', '.lib', '.a', '.bin', '.dkey', '.key', '.ird'}:
                    raise ValueError('PS3 executable, image or object: ' + name)
                if PurePosixPath(name).name.casefold()=='redump_dkey.7z' or 'iso-private' in parts:
                    raise ValueError('Private disc-key cache in package: ' + name)
                if any(part in {'recompiled', 'raw_jobs', 'spu_posttask_native', 'spu_gameplay'} for part in parts):
                    raise ValueError('Generated game source: ' + name)
                with archive.open(entry) as stream:
                    magic = stream.read(4)
                if magic in {b'\x7fELF', b'SCE\0'}:
                    raise ValueError('Disguised PS3 executable: ' + name)
                if kind != 'program' and (suffix in {'.exe', '.dll'} or magic[:2] == b'MZ'):
                    raise ValueError('Executable in language data: ' + name)
            if kind == 'program':
                program = json.loads(archive.read('program/program-manifest.json'))
                for item in program['files']:
                    content = archive.read('program/' + item['path'])
                    if len(content) != item['bytes'] or digest(content) != item['sha256']:
                        raise ValueError('Program member identity mismatch')
                listed = {item['path'] for item in program['files']}
                members = {n[len('program/'):] for n in archive.namelist() if n.startswith('program/') and not n.endswith('/')}
                if members - listed - {'program-manifest.json'}:
                    raise ValueError('Unlisted program members: ' + ', '.join(sorted(members - listed - {'program-manifest.json'})[:5]))
                # Shader seed / PSO inventory are allowed only as one complete verified set.
                shader_prep = stage_program_assets.check_package_rows(listed, lambda rel: archive.read('program/' + rel))
                if bool(shader_prep) != bool(program.get('shader_prep')):
                    raise ValueError('program-manifest shader_prep block does not match packaged assets')
                if shader_prep and program['shader_prep']['manifest_sha256'] != shader_prep['manifest_sha256']:
                    raise ValueError('program-manifest shader_prep identity mismatch')
        files.append(dict(file=path.name, bytes=len(raw), sha256=digest(raw), members=len(names)))
    return dict(passed=True, version=manifest['version'], mode=manifest['mode'],
                languages=manifest['languages'], files=files,
                total_package_bytes=sum(row['bytes'] for row in files),
                contains_precompiled_windows_game=True,
                contains_complete_ps3_elf=False, contains_disc_image=False,
                contains_generated_ppu_spu_sources=False,
                copyright_clearance_claimed=False, gameplay_validation_claimed=False,
                shader_prep=shader_prep, contains_local_gpu_pipeline_library=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    args = parser.parse_args()
    result = audit(args.folder)
    (args.folder / 'PACKAGE-AUDIT.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))
