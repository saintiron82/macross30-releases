"""Build an isolated, explicitly partial English preview from Japanese resources."""
import json
import os
from pathlib import Path
import shutil
import struct
from datetime import datetime, timezone

from english_pack import ROOT, digest, write
from stage_gop_primary_mirrors import pidx, read_payload, fsts_entries
from gop_strings import parse


def main():
    source = ROOT / 'native/vfs'
    preview = ROOT / 'build/english-preview'
    if preview.exists():
        raise FileExistsError('Preview already exists; refusing to overwrite it')
    pack_rel = Path('PS3_GAME/USRDIR/DATA/PACK')
    pack = source / pack_rel / 'DATA.DAT'
    index = source / pack_rel / 'PACK.IDX'
    _, master, archives = pidx(index)
    _, embedded, embedded_archives = pidx(pack)
    originals, replacements = {}, {}
    for catalog_file in sorted((ROOT / 'localization/en-US/structured').glob('*.json')):
        cat = json.loads(catalog_file.read_text(encoding='utf8'))
        if not any(r['translation'] for r in cat['entries']):
            continue
        name = Path(cat['source_file']).name
        original = (ROOT / cat['source_file']).read_bytes()
        if digest(original) != cat['source_sha256']:
            raise ValueError('Source hash changed')
        replacement = (ROOT / 'localization/en-US/build/gop' / name).read_bytes()
        expected = {r['index']: r['translation'] for r in cat['entries'] if r['translation']}
        for before, after in zip(parse(original), parse(replacement)):
            if after['text'] != expected.get(before['index'], before['text']):
                raise ValueError('English output differs from catalog')
        originals[name], replacements[name] = original, replacement
    arc = [r for r in archives if r['name'] == 'gop']
    earc = [r for r in embedded_archives if r['name'] == 'gop']
    if len(arc) != 1 or len(earc) != 1 or any(arc[0][k] != earc[0][k] for k in ('pack', 'offset', 'size')):
        raise ValueError('GOP archive aliases differ')
    archive, _ = read_payload(pack, arc[0])
    rebuilt = bytearray(archive)
    table = struct.unpack_from('<I', archive, 8)[0]
    found = set()
    for row in fsts_entries(archive):
        name = Path(row['name']).name
        if name not in replacements:
            continue
        if row['name'] != 'data/gop/' + name or row['payload'] != originals[name] or name in found:
            raise ValueError('Unexpected GOP archive source')
        found.add(name)
        rebuilt.extend(b'\0' * (-len(rebuilt) % 16))
        offset = len(rebuilt)
        rebuilt.extend(replacements[name])
        struct.pack_into('<3I', rebuilt, table + row['entry'] * 16 + 4, offset, len(replacements[name]), 0)
    if found != set(replacements):
        raise ValueError('Missing secondary GOP targets')
    for name in replacements:
        for rows in (master, embedded):
            matches = [r for r in rows if r['name'] == name]
            if not matches:
                raise ValueError('Missing primary GOP alias')
            for row in matches:
                if Path(row['pack']).name.lower() != 'data.dat' or read_payload(pack, row)[0] != originals[name]:
                    raise ValueError('Unexpected primary GOP source')
    # Immutable disc files are shared; every modified file is an independent copy.
    for path in sorted((source / 'PS3_GAME').rglob('*')):
        if not path.is_file():
            continue
        relative = path.relative_to(source)
        target = preview / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if relative in (pack_rel / 'DATA.DAT', pack_rel / 'PACK.IDX'):
            shutil.copyfile(path, target)
        else:
            os.link(path, target)
    shutil.copyfile(source / 'PS3_DISC.SFB', preview / 'PS3_DISC.SFB')
    output_pack, output_index = preview / pack_rel / 'DATA.DAT', preview / pack_rel / 'PACK.IDX'
    records = []
    with output_pack.open('r+b') as data_file, output_index.open('r+b') as index_file:
        def append(payload):
            data_file.seek(0, 2)
            data_file.write(b'\0' * (-data_file.tell() % 2048))
            offset = data_file.tell()
            data_file.write(payload)
            return offset
        for name, payload in replacements.items():
            offset = append(payload)
            for stream, rows in ((index_file, master), (data_file, embedded)):
                for row in rows:
                    if row['name'] == name:
                        stream.seek(row['record'] + 12)
                        stream.write(struct.pack('<3I', offset, len(payload), 0))
            records.append(dict(name=name, sha256=digest(payload)))
        offset = append(rebuilt)
        for stream, row in ((index_file, arc[0]), (data_file, earc[0])):
            stream.seek(row['record'] + 8)
            stream.write(struct.pack('<2I', offset, len(rebuilt)))
    for metadata in (output_index, output_pack):
        _, rows, arcs = pidx(metadata)
        for name, expected in replacements.items():
            for row in [r for r in rows if r['name'] == name]:
                if read_payload(output_pack, row)[0] != expected:
                    raise ValueError('Preview primary verification failed')
        row = next(r for r in arcs if r['name'] == 'gop')
        if read_payload(output_pack, row)[0] != rebuilt:
            raise ValueError('Preview secondary verification failed')
    state = ROOT / 'native/state-en-preview'
    if state.exists():
        raise FileExistsError(state)
    # The guest reads these installation resources; no mutable saves are linked.
    installed = state / 'hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack'
    installed.mkdir(parents=True)
    for path in (preview / pack_rel).iterdir():
        os.link(path, installed / path.name.lower())
    (state / 'hdd1').mkdir()
    subtitles = ROOT / 'localization/en-US/subtitles/preview'
    subtitles.mkdir(parents=True, exist_ok=True)
    review = subtitles.parent / 'review'
    for path in review.glob('*.en.srt'):
        shutil.copyfile(path, subtitles / path.name)
    lines = (review / 'tracks.tsv').read_text(encoding='utf8').splitlines()
    tracks = [lines[0]]
    for line in lines[1:]:
        fields = line.split('\t')
        fields[3] = '1'
        tracks.append('\t'.join(fields))
    (subtitles / 'tracks.tsv').write_bytes(('\n'.join(tracks) + '\n').encode('utf8'))
    run = ROOT / 'native/analysis' / ('english-preview-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
    run.mkdir(parents=True)
    saves = ROOT / 'native/play/gamedata'
    if saves.exists():
        shutil.copytree(saves, run / 'gamedata')
    elf = ROOT / 'localization/en-US/build/EBOOT.ui-en.elf'
    manifest = json.loads((elf.parent / 'manifest.json').read_text(encoding='utf8'))
    if digest(elf.read_bytes()) != manifest['output_sha256']:
        raise ValueError('English ELF checksum mismatch')
    command = ['python', str(ROOT / 'tools/play/play_native.py'), '--language', 'en-US',
               '--elf', str(elf), '--vfs', str(preview), '--run-directory', str(run)]
    for key, value in dict(PS3_HDD0_ROOT=str(state/'hdd0'), PS3_HDD1_ROOT=str(state/'hdd1'),
                           PS3_GAMEDATA_ROOT=str(run/'gamedata'), MACROSS_SUBTITLE_DIR=str(subtitles),
                           MACROSS_BATTLE_SUBTITLES='0').items():
        command.extend(['--env', key + '=' + value])
    write(preview / 'preview.json', dict(status='PARTIAL_ENGLISH_PREVIEW', command=command,
                                        run_directory=str(run), gop_payloads=records,
                                        elf_slots=manifest['translated_slots'],
                                        enabled_movie_tracks=len(tracks)-1,
                                        game_render_verified=False, saves='independent copy'))
    print(json.dumps(dict(preview=str(preview), run=str(run), english_elf_slots=manifest['translated_slots'],
                          english_gop_families=len(records), enabled_movies=len(tracks)-1)))


if __name__ == '__main__':
    main()
