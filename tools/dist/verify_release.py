"""Verify final release ZIP contents without extracting or running the game."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

from release_profile import STABLE_ENV, NORMALIZED_ENV


def digest(stream):
    h = hashlib.sha256()
    for block in iter(lambda: stream.read(8 << 20), b''):
        h.update(block)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', type=Path)
    args = parser.parse_args()
    root = args.target.resolve()
    build = json.loads((root / 'work/make-dist-report.json').read_text(encoding='utf-8'))
    assert build['env_check_ok'] is True
    files = {}
    for name in build['packages']:
        path = root / 'packages' / name
        with path.open('rb') as stream:
            files[name] = dict(bytes=path.stat().st_size, sha256=digest(stream))
    release_version = build['release_version']
    program = root / 'packages' / f"Macross30-program-{release_version}.zip"
    korean = root / 'packages' / f"Macross30-korean-{release_version}.zip"
    prefix = 'Macross30/'
    with zipfile.ZipFile(program) as z:
        manifest = json.loads(z.read(prefix + 'program-manifest.json'))
        assert manifest['release_version'] == release_version
        expected = {prefix + row['path'] for row in manifest['files']} | {prefix + 'program-manifest.json'}
        assert len(z.namelist()) == len(expected) and set(z.namelist()) == expected
        for row in manifest['files']:
            name = prefix + row['path']
            assert z.getinfo(name).file_size == row['size'], name
            with z.open(name) as stream:
                assert digest(stream) == row['sha256'], name
        profile = json.loads(z.read(prefix + 'release-profile.json'))
        policy = {'stable': STABLE_ENV, 'normalized': NORMALIZED_ENV}[profile['name']]
        assert profile['environment'] == policy
        launcher = z.read(prefix + 'Macross30.cmd').decode('utf-8')
        for key, value in policy.items():
            assert f'set "{key}={value}"' in launcher
        assert 'bin\\subtitles' in launcher
        assert hashlib.sha256(z.read(prefix + 'bin/Macross30Recompiled.exe')).hexdigest() == profile['binary_provenance']['exe_sha256']
        assert f'set "MACROSS_BATTLE_SUBTITLES={int(profile["binary_provenance"]["battle_subtitles"])}"' in launcher
        elf_sha = hashlib.sha256(z.read(prefix + 'bin/EBOOT.ko.elf')).hexdigest()
        registry = z.read(prefix + 'bin/subtitles/tracks.tsv')
        assert hashlib.sha256(registry).hexdigest() == profile['subtitle_registry_sha256']
        tracks = registry.decode('utf-8').splitlines()[1:]
        assert len(tracks) == profile['subtitle_tracks'] == 55
        for line in tracks:
            movie, srt, origin, enabled, source_sha, srt_sha, status = line.split('\t')
            assert enabled == '1'
            assert hashlib.sha256(z.read(prefix + 'bin/subtitles/' + srt)).hexdigest() == srt_sha
        assert not any('/play/' in n or '/data/' in n or '/firmware/' in n or
                       Path(n).suffix.lower() in ('.pam', '.wav', '.log', '.pdb', '.dmp') for n in z.namelist())
    with zipfile.ZipFile(korean) as z:
        pack = json.loads(z.read(prefix + 'korean/korean-pack.json'))
        assert pack['release_version'] == release_version
        assert pack['elf_sha256'] == elf_sha
        version = z.read(prefix + 'korean/data-version.txt').decode('utf-8').strip()
        assert version == manifest['data_version'] == build['data_version']
        plan = z.read(prefix + 'korean/install-plan.tsv').decode('utf-8').replace('\r\n', '\n')
        assert hashlib.sha256(plan.encode('utf-8')).hexdigest()[:16] == version
        expected = {prefix + 'korean/' + name for name in ('korean-pack.json', 'data-version.txt', 'install-plan.tsv')}
        for row in pack['korean_files']:
            name = prefix + row['delta']
            expected.add(name)
            assert hashlib.sha256(z.read(name)).hexdigest() == row['delta_sha256']
        assert len(z.namelist()) == len(expected) and set(z.namelist()) == expected
    report = dict(status='PASS', release_version=release_version, packages=files, program_version=build['program_version'],
                  data_version=version, subtitle_tracks=len(tracks), environment=policy,
                  program_files=len(manifest['files']), delta_files=len(pack['korean_files']),
                  archive_members_and_checksums_verified=True, game_launched=False)
    (root / 'work/archive-verification.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    (root / 'packages/SHA256SUMS.txt').write_text(
        ''.join(row['sha256'] + '  ' + name + '\n' for name, row in files.items()), encoding='ascii')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
