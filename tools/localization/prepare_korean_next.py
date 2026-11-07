"""Build a new, isolated 826-draft Korean test disc and HDD state. Never launch it."""
import hashlib
import argparse
import json
import os
import re
import shutil
import struct
import zlib
import unicodedata
from pathlib import Path

import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from analyze_archives import cstring, unpack
from gop_strings import patch, parse
from prepare_korean_vfs import sha_file, live_processes
from apply_korean_font_state import verify_payloads

ROOT = Path(__file__).resolve().parents[2]
ART = ROOT / 'build/font-ko-next'
SOURCE_STATE = ROOT / 'native/state-ko'
STATE = ROOT / 'native/state-ko-next'
SOURCE_VFS = ROOT / 'build/game-ko'
VFS = ROOT / 'build/game-ko-next'
INSTALL = Path('hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack')
DISC = Path('PS3_GAME/USRDIR/DATA/PACK')
EXPECTED_DRAFTS = 826
NUMERIC_EQUIVALENCES = {}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def archive_entries(blob):
    if blob[:4] != b'FSTS':
        raise ValueError('Not FSTS')
    count, table, names, names_size = struct.unpack_from('<4I', blob, 4)
    result = []
    for i in range(count):
        no, off, size, packed = struct.unpack_from('<4I', blob, table + 16*i)
        encoded = blob[off:off + (packed if 0 < packed < size else size)]
        raw = unpack(encoded, size) if 0 < packed < size else encoded
        result.append((cstring(blob[names:names+names_size], no), no, off, size, packed, encoded, raw))
    return table, result


def build_gop():
    original = (ROOT / 'analysis/gop_archive.fsts').read_bytes()
    table, entries = archive_entries(original)
    groups = {}
    for path in sorted((ROOT / 'localization/ko-KR/structured').glob('*.json')):
        catalog = json.loads(path.read_text(encoding='utf-8'))
        translated = [row for row in catalog['entries'] if row.get('translation')]
        if not translated:
            continue
        source = ROOT / catalog['source_file']
        if sha_file(source) != catalog['source_sha256']:
            raise ValueError('Catalog source hash changed')
        original_rows = parse(source.read_bytes())
        for row in catalog['entries']:
            if row['id'] != f'{source.stem}:string:{row["index"]}':
                raise ValueError('Catalog stable ID mismatch')
            original_row = original_rows[row['index']]
            if row['offset'] != original_row['offset'] or row['source'] != original_row['text']:
                raise ValueError('Catalog original source/index/offset mismatch')
        for row in translated:
            numeric = lambda s: re.findall(r'[-+]?\d+(?:\.\d+)?', unicodedata.normalize('NFKC', s))
            buttons = lambda s: re.findall(r'[RL][123]|[○×△□]|X(?=ボタン| 버튼)', unicodedata.normalize('NFKC', s))
            if buttons(row['source']) != buttons(row['translation']):
                raise ValueError(f'Buttons differ: {row["id"]}')
            if numeric(row['source']) != numeric(row['translation']):
                equivalence = NUMERIC_EQUIVALENCES.get(row['id'], {})
                if (equivalence.get('source_sha256') != sha(row['source'].encode('utf-8')) or
                        equivalence.get('translation_sha256') != sha(row['translation'].encode('utf-8')) or
                        not equivalence.get('rationale')):
                    raise ValueError(f'Unreviewed numerical representation difference: {row["id"]}')
        groups[source.name] = (source.read_bytes(), translated)
    if sum(len(rows) for _, rows in groups.values()) != EXPECTED_DRAFTS:
        raise ValueError(f'Expected exactly {EXPECTED_DRAFTS} current draft strings')
    output = bytearray(original[:min(e[2] for e in entries)])
    expected, changed = [], []
    for i, (name, no, off, size, packed, encoded, raw) in enumerate(entries):
        filename = Path(name).name
        if filename in groups:
            source, rows = groups[filename]
            if raw != source:
                raise ValueError('Original GOP archive/source mismatch')
            raw, indices = patch(raw, rows)
            reparsed = parse(raw)
            if indices != [r['index'] for r in rows]:
                raise ValueError('Patched string indices differ')
            for row in rows:
                if reparsed[row['index']]['text'] != row['translation']:
                    raise ValueError('Translation round trip failed')
            encoded = zlib.compress(raw, 9)
            packed = len(encoded)
            if packed >= len(raw):
                encoded, packed = raw, 0
            changed.append(dict(file=filename, strings=len(indices), indices=indices, sha256=sha(raw)))
        output.extend(bytes(-len(output) % 16))
        off = len(output)
        output.extend(encoded)
        struct.pack_into('<4I', output, table + 16*i, no, off, len(raw), packed)
        expected.append(raw)
    output.extend(bytes(-len(output) % 16))
    _, actual = archive_entries(bytes(output))
    if [e[6] for e in actual] != expected or [e[0] for e in actual] != [e[0] for e in entries]:
        raise ValueError('GOP archive payload/name/order verification failed')
    report = dict(strings=EXPECTED_DRAFTS, patched=changed, verified_payloads=len(actual),
                  unchanged_payloads=len(actual)-len(changed), sha256=sha(output), size=len(output),
                  tokens_and_ids_preserved=True, runtime_verified=False,
                  numerical_representation_equivalences=NUMERIC_EQUIVALENCES)
    return bytes(output), report


def references(blob):
    if blob[:4] != b'PIDX':
        raise ValueError('Not PIDX')
    f = struct.unpack_from('<9I', blob, 4)
    secondary, names, names_size = f[5], f[7], f[8]
    result = {}
    for i in range(struct.unpack_from('<I', blob, secondary)[0]):
        record = secondary + struct.unpack_from('<I', blob, secondary + 4 + 4*i)[0]
        no = struct.unpack_from('<I', blob, record)[0]
        name = cstring(blob[names:names+names_size], no).lower()
        if name in result and name in ('resident', 'gop'):
            raise ValueError('Duplicate PIDX archive name')
        result[name] = record + 8
    return result


def independent_copy(source, target):
    before = source.stat()
    target.parent.mkdir(parents=True, exist_ok=True)
    with source.open('rb') as inp, target.open('xb') as out:
        shutil.copyfileobj(inp, out, 8*1024*1024)
        out.flush()
        os.fsync(out.fileno())
    digest = sha_file(source)
    if sha_file(target) != digest or target.stat().st_nlink != 1 or os.path.samefile(source, target):
        raise ValueError(f'Independent copy failed: {target}')
    after = source.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError('Source changed during copy')
    return dict(path=str(target.relative_to(ROOT)), source=str(source.relative_to(ROOT)),
                size=after.st_size, sha256=digest, storage='independent copy')


def main():
    global ART, STATE, VFS, EXPECTED_DRAFTS, NUMERIC_EQUIVALENCES
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifacts', type=Path, default=ART)
    parser.add_argument('--state', type=Path, default=STATE)
    parser.add_argument('--vfs', type=Path, default=VFS)
    parser.add_argument('--expected-drafts', type=int, default=EXPECTED_DRAFTS)
    parser.add_argument('--numeric-equivalences', type=Path)
    args = parser.parse_args()
    ART, STATE, VFS = (p.resolve() for p in (args.artifacts, args.state, args.vfs))
    EXPECTED_DRAFTS = args.expected_drafts
    if args.numeric_equivalences:
        NUMERIC_EQUIVALENCES = json.loads(args.numeric_equivalences.read_text(encoding='utf-8'))['entries']
    font_details = json.loads((ART/'manifest.json').read_text(encoding='utf-8'))
    for destination in (STATE, VFS):
        if destination.exists():
            raise FileExistsError(f'Refusing to overwrite {destination}')
    for root in (SOURCE_STATE, SOURCE_VFS, ART, STATE.parent, VFS.parent):
        if root.resolve() != root.absolute() or not root.resolve().is_relative_to(ROOT.resolve()):
            raise ValueError('Scoped trees must not be redirected')
    processes = live_processes()
    for process in processes:
        command = (process.get('CommandLine') or '').replace('\\', '/').lower()
        if not command or any(str(p).replace('\\', '/').lower() in command for p in (SOURCE_STATE, STATE, SOURCE_VFS, VFS)):
            raise ValueError('A native process may be using the source/destination KO scope')
    source_files = sorted(p for p in SOURCE_STATE.rglob('*') if p.is_file())
    data_source = SOURCE_STATE / INSTALL / 'data.dat'
    required = sum(p.stat().st_size for p in source_files) + data_source.stat().st_size + 256*1024*1024
    free = shutil.disk_usage(ROOT).free
    if free < required:
        raise OSError(f'Insufficient disk space: {free} free, {required} required')
    gop, gop_report = build_gop()
    font_manifest = json.loads((ART / 'resident-segmented-manifest.json').read_text(encoding='utf-8'))
    font = (ART / 'resident-segmented.fsts').read_bytes()
    if sha(font) != font_manifest['sha256'] or font_manifest['codec_changed_entries']:
        raise ValueError('Font archive manifest mismatch')
    verify_payloads(font, font_manifest)
    copies = []
    STATE.mkdir()
    for source in source_files:
        copies.append(independent_copy(source, STATE / source.relative_to(SOURCE_STATE)))
    for source in SOURCE_STATE.rglob('*'):
        if source.is_dir():
            (STATE / source.relative_to(SOURCE_STATE)).mkdir(parents=True, exist_ok=True)
    data, index = STATE / INSTALL / 'data.dat', STATE / INSTALL / 'pack.idx'
    original_index = index.read_bytes()
    master = bytearray(original_index)
    with data.open('r+b') as stream:
        h = stream.read(40)
        f = struct.unpack_from('<9I', h, 4)
        stream.seek(0)
        embedded = bytearray(stream.read(f[7]+f[8]))
        data_refs, master_refs = references(embedded), references(master)
        changes = []
        old_length = data.stat().st_size
        for name, payload in (('resident', font), ('gop', gop)):
            di, mi = data_refs[name], master_refs[name]
            old = bytes(embedded[di:di+8])
            if old != bytes(master[mi:mi+8]):
                raise ValueError('Disc/HDD embedded/master archive references disagree')
            old_off, old_size = struct.unpack('<II', old)
            stream.seek(old_off)
            previous = stream.read(old_size)
            if name == 'resident':
                previous_manifest = json.loads((ROOT/'build/font-ko/resident-segmented-manifest.json').read_text())
                expected = previous_manifest['sha256']
            else:
                expected = json.loads((ROOT/'analysis/archive_validation.json').read_text())['sha256']
            if sha(previous) != expected:
                raise ValueError(f'Current KO {name} archive hash mismatch')
            stream.seek(0, 2)
            end = stream.tell()
            offset = (end+2047)//2048*2048
            stream.write(bytes(offset-end))
            stream.write(payload)
            new = struct.pack('<II', offset, len(payload))
            embedded[di:di+8], master[mi:mi+8] = new, new
            changes.append(dict(archive=name, old_reference=old.hex(), new_reference=new.hex(),
                                embedded_field=di, master_field=mi, offset=offset,
                                size=len(payload), sha256=sha(payload)))
        for change in changes:
            stream.seek(change['embedded_field'])
            stream.write(bytes.fromhex(change['new_reference']))
        stream.flush(); os.fsync(stream.fileno())
    with index.open('r+b') as stream:
        stream.write(master); stream.flush(); os.fsync(stream.fileno())
    if index.read_bytes() != master:
        raise ValueError('Index write verification failed')
    # Compare every original DATA byte while masking only the two intended references.
    before_hash, after_hash = hashlib.sha256(), hashlib.sha256()
    with data_source.open('rb') as before, data.open('rb') as after:
        offset = 0
        while offset < old_length:
            a = before.read(min(8*1024*1024, old_length-offset))
            b = bytearray(after.read(len(a)))
            for change in changes:
                field = change['embedded_field']
                if offset <= field and field+8 <= offset+len(a):
                    b[field-offset:field-offset+8] = bytes.fromhex(change['old_reference'])
            if a != b:
                raise ValueError('Unexpected modification outside PIDX references')
            before_hash.update(a); after_hash.update(b); offset += len(a)
        for change in changes:
            after.seek(change['offset'])
            payload = after.read(change['size'])
            if sha(payload) != change['sha256']:
                raise ValueError('Appended archive hash mismatch')
            if change['archive'] == 'resident':
                verify_payloads(payload, font_manifest)
            else:
                _, actual = archive_entries(payload)
                if len(actual) != gop_report['verified_payloads']:
                    raise ValueError('GOP installed verification failed')
    VFS.mkdir()
    assets = []
    for source in sorted((SOURCE_VFS/'PS3_GAME').rglob('*')) + [SOURCE_VFS/'PS3_DISC.SFB']:
        if not source.is_file():
            continue
        relative = source.relative_to(SOURCE_VFS)
        target = VFS / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if relative.as_posix().lower() in ('ps3_game/usrdir/data/pack/data.dat', 'ps3_game/usrdir/data/pack/pack.idx'):
            assets.append(independent_copy(STATE/INSTALL/source.name.lower(), target))
        else:
            os.link(source, target)
            assets.append(dict(path=str(target.relative_to(ROOT)), source=str(source.relative_to(ROOT)),
                               size=source.stat().st_size, sha256=sha_file(source), storage='shared read-only hard link'))
            if not os.path.samefile(source, target):
                raise ValueError('Hard link verification failed')
    sizes = []
    for name in ('pack.idx','data.dat','data2.dat','fileset0.dat','fileset1.dat','fileset2.dat','init.dat','shaders.dat'):
        hdd, disc = STATE/INSTALL/name, VFS/DISC/name
        if hdd.stat().st_size != disc.stat().st_size or sha_file(hdd) != sha_file(disc):
            raise ValueError(f'Installed/disc mismatch: {name}')
        sizes.append(dict(name=name, size=hdd.stat().st_size, sha256=sha_file(hdd), disc_hdd_equal=True))
    (ART/'gop_archive.fsts').write_bytes(gop)
    report = dict(state=str(STATE), vfs=str(VFS), translated_draft_strings=EXPECTED_DRAFTS,
                  font_total_glyphs=font_details['total_glyph_count'],
                  font_hangul_glyphs=font_details['new_hangul_count'], gop=gop_report,
                  archive_changes=changes, final_data_length=data.stat().st_size,
                  baseline_data_masked_sha256=before_hash.hexdigest(),
                  final_data_masked_sha256=after_hash.hexdigest(), independent_state_files=copies,
                  vfs_assets=assets, guest_install_size_and_hash_checks=sizes,
                  shared_assets_semantics='All shared hard-linked VFS assets are read-only. Copy before editing; never chmod/patch shared links.',
                  source_writes=False, runtime_verified=False, native_processes=processes,
                  disk_free_before=free, required_bytes_before=required)
    (ART/'next-preparation-manifest.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    for target in (STATE/'korean_next_manifest.json', VFS/'READ_ONLY_SHARED_ASSETS.json'):
        target.write_text(json.dumps(dict(manifest=str(ART/'next-preparation-manifest.json'),
            shared_semantics=report['shared_assets_semantics'], runtime_verified=False),indent=2),encoding='utf-8')
    print(f'Prepared independent {STATE} and {VFS}; {EXPECTED_DRAFTS} drafts, '
          f'{font_details["new_hangul_count"]} Hangul; all eight installed/disc hashes and sizes match.')


if __name__ == '__main__':
    main()
