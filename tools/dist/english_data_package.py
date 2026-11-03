"""Add frozen English v11 as original-ISO resource/ELF deltas, preserving KO/JA."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import zipfile

from make_dist import make_delta, read_delta_header, apply_delta_sha

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/local_build'))
from elf_patch import apply


def sha(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def build(base_archive, output, version='0.3.0', block=2048, expected_epoch='v11'):
    base_archive, output = Path(base_archive), Path(output)
    frozen = json.loads((ROOT / 'localization/en-US/pack-ready.json').read_text(encoding='utf8'))
    if expected_epoch not in ('v11', 'v13', 'v14'):
        raise ValueError('Unreviewed English epoch')
    if frozen['locale'] != 'en-US' or Path(frozen['vfs']).name != 'english-preview-images-' + expected_epoch:
        raise ValueError('Expected the explicitly selected frozen English ' + expected_epoch)
    proofs = {row['path'].replace('\\', '/').lower(): row['sha256'] for row in frozen['proofs']}
    vfs = ROOT / frozen['vfs']
    subtitles = ROOT / frozen['subtitles']
    cache = ROOT / ('native/analysis/english-dist-' + expected_epoch + '-deltas')
    cache.mkdir(parents=True, exist_ok=True)
    additions = {}
    changes = {}
    with zipfile.ZipFile(base_archive) as old:
        recipe = json.loads(old.read('payload/recipe.json'))
        original = ROOT / 'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf'
        if sha(original) != recipe['original_elf_sha256']:
            raise ValueError('Original ELF identity mismatch')
        english_elf = ROOT / frozen['elf']
        english_elf_sha = sha(english_elf)
        if english_elf_sha != proofs[frozen['elf'].lower()]:
            raise ValueError('Frozen English ELF changed')
        elf_delta, _ = make_delta(original, english_elf, recipe['original_elf_sha256'], english_elf_sha, block)
        if hashlib.sha256(apply(original.read_bytes(), elf_delta)).hexdigest() != english_elf_sha:
            raise ValueError('English ELF delta readback failed')
        additions['payload/elf/en-US.mkdelta'] = elf_delta
        lines = []
        for line in old.read('payload/plans/ja-JP.tsv').decode('utf8').splitlines():
            if not line or line.startswith('#'):
                lines.append('# English ' + expected_epoch + ' original-data delta install plan' if line.startswith('#') else line)
                continue
            fields = line.split('\t'); fields += [''] * (6 - len(fields))
            kind, target, size, digest, source, delta = fields[:6]
            if kind == 'iso' and '/DATA/PACK/' in target:
                translated = vfs / source
                relative = translated.relative_to(ROOT).as_posix()
                expected = proofs.get(relative.lower())
                if expected is None:
                    raise ValueError('Frozen pack proof missing: ' + relative)
                actual = sha(translated)
                if actual != expected:
                    raise ValueError('Frozen English pack changed: ' + relative)
                if actual != digest:
                    pristine = ROOT / 'native/vfs' / source
                    if pristine.stat().st_size != int(size) or sha(pristine) != digest:
                        raise ValueError('Original ISO resource identity mismatch: ' + source)
                    name = 'english-' + expected_epoch + '/' + translated.name.lower() + '.mkdelta'
                    delta_path = cache / (actual[:16] + '-' + translated.name.lower() + '.mkdelta')
                    valid = False
                    if delta_path.exists():
                        info = read_delta_header(delta_path.read_bytes())
                        valid = info['source_sha'] == digest and info['target_sha'] == actual
                    if not valid:
                        print('ENGLISH_DELTA: ' + translated.name, flush=True)
                        data, stats = make_delta(pristine, translated, digest, actual, block)
                        delta_path.write_bytes(data)
                    count, reproduced = apply_delta_sha(delta_path.read_bytes(), pristine)
                    if count != translated.stat().st_size or reproduced != actual:
                        raise ValueError('English resource delta readback failed: ' + translated.name)
                    additions['payload/' + name] = delta_path
                    changes[target.lower()] = (translated.stat().st_size, actual)
                    fields = ['delta', target, str(translated.stat().st_size), actual, source, name]
            elif kind == 'copy' and source.lower() in changes:
                new_size, new_sha = changes[source.lower()]
                fields[2:4] = [str(new_size), new_sha]
            lines.append('\t'.join(fields))
        plan = ('\n'.join(lines) + '\n').encode()
        additions['payload/plans/en-US.tsv'] = plan
        data_id = hashlib.sha256(plan).hexdigest()[:16]
        recipe['version'] = version
        recipe['mode'] = 'user-original-eboot'
        recipe['languages']['en-US'] = dict(elf_sha256=english_elf_sha, elf_patch='elf/en-US.mkdelta', plan='plans/en-US.tsv',
                                          data_id=data_id, subtitle_dir='subtitles-en',
                                          battle_catalog='subtitles-en/battle.en.tsv', review_status='full_draft_review_pending', frozen_epoch=expected_epoch)
        additions['payload/recipe.json'] = json.dumps(recipe, indent=2).encode()
        for file in sorted(subtitles.iterdir()):
            if file.is_file() and file.suffix in ('.srt', '.tsv'):
                relative = file.relative_to(ROOT).as_posix()
                if proofs.get(relative.lower()) != sha(file):
                    raise ValueError('Frozen English subtitle changed or unproved: ' + relative)
                additions['payload/subtitles-en/' + file.name] = file
        report = dict(version=version, english_epoch=expected_epoch, english_elf_sha256=english_elf_sha,
                      changed_resources=changes, data_id=data_id, full_elf_distributed=False)
        additions['payload/english-' + expected_epoch + '-distribution.json'] = json.dumps(report, indent=2).encode()
        output.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for entry in old.infolist():
                if entry.filename not in additions:
                    archive.writestr(entry.filename, old.read(entry))
            for name, value in additions.items():
                if isinstance(value, Path):
                    archive.write(value, name)
                else:
                    archive.writestr(name, value)
    print(json.dumps(dict(path=str(output), bytes=output.stat().st_size, sha256=sha(output), changed_resources=len(changes), languages=list(recipe['languages'])), indent=2))
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-archive', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--version', default='0.3.0')
    parser.add_argument('--expected-epoch', choices=('v11', 'v13', 'v14'), default='v11')
    args = parser.parse_args()
    build(args.base_archive, args.output, args.version, expected_epoch=args.expected_epoch)
