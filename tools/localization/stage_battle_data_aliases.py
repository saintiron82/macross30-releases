"""Review/stage eight explicit battle DATA rows; never modify source resources.

Default writes a plan only. --apply requires a NEW --output directory.
Every duplicated basename is anchored by the ISO tuple and master record.
"""
import argparse
import json
from pathlib import Path
import struct

import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
import stage_ui_images as ui

ROI = (112, 101, 84, 20)


def transplant_variant(original, replacement, roi=ROI):
    ui.validate_dds(original)
    ui.validate_dds(replacement)
    if len(original) != len(replacement) or original[:128] != replacement[:128]:
        raise ValueError('Variant DDS header/size changed')
    width = struct.unpack_from('<I', original, 16)[0]
    height = struct.unpack_from('<I', original, 12)[0]
    x, y, w, h = roi
    if x < 0 or y < 0 or x+w > width or y+h > height:
        raise ValueError('Variant ROI outside DDS')
    result = bytearray(original)
    for yy in range(y, y+h):
        at = 128+4*(yy*width+x)
        result[at:at+4*w] = replacement[at:at+4*w]
    for at in range(len(original)):
        pi = (at-128)//4
        inside = at >= 128 and x <= pi % width < x+w and y <= pi//width < y+h
        if not inside and result[at] != original[at]:
            raise ValueError('Variant changed outside translation ROI')
    return bytes(result)


def plan(bundle, index, variant_output):
    bundle, index = Path(bundle).resolve(), Path(index).resolve()
    epoch_path = bundle/'source-state.json'
    epoch_bytes = epoch_path.read_bytes()
    epoch = json.loads(epoch_bytes)
    baseline = {r['filename']: r for r in epoch['files'] if r['role'] == 'disc'}
    source = Path(baseline['data.dat']['path']).resolve()
    source_hash = baseline['data.dat']['sha256']
    if ui.sha_file(source) != source_hash:
        raise ValueError('DATA differs from reviewed source epoch')
    first_stage = json.loads((bundle/'staging-data2-result.json').read_text())
    index_hash = ui.sha_file(index)
    if index_hash != first_stage['output_index_sha256']:
        raise ValueError('Intermediate index differs from reviewed DATA2 stage')
    provenance = json.loads((bundle/'source-provenance.json').read_text())['sources']
    rows = [r for r in provenance if r['path'] in ('quest_title_01_01.dds', 'ope29.dds')]
    aliases = json.loads((bundle/'duplicate-original-analysis.json').read_text())['duplicates']
    rows += [dict(r, path=r['name']) for r in aliases if Path(r['pack']).name.lower() == 'data.dat']
    if len(rows) != 8:
        raise ValueError('Expected exactly eight reviewed DATA rows')
    if len({(r['path'], r['offset'], r['size'], r['compressed_size']) for r in rows}) != 8:
        raise ValueError('Duplicate source tuple in explicit row list')
    cursor = source.stat().st_size
    changes = []
    variant_proof = None
    with source.open('rb') as f:
        for row in rows:
            name = row['path']
            selection = {name: row}
            master_blob, master = ui.read_pidx(index, [name], selection)
            embedded_blob, embedded = ui.read_pidx(source, [name], selection)
            m, e = master[name], embedded[name]
            if 'record' in row and m['record'] != row['record']:
                raise ValueError('Master row location differs from ISO provenance')
            ui.bounded_range(row['offset'], row['compressed_size'] or row['size'], source.stat().st_size)
            f.seek(row['offset'])
            encoded = f.read(row['compressed_size'] or row['size'])
            if ui.sha(encoded) != row['encoded_sha256']:
                raise ValueError('Original encoded payload changed')
            raw = ui.unpack(encoded, row['size']) if row['compressed_size'] else encoded
            if ui.sha(raw) != row['sha256']:
                raise ValueError('Original DDS changed')
            replacement = bundle/'replacements'/name
            payload = replacement.read_bytes()
            ui.validate_dds(payload)
            if payload[:128] != raw[:128] or len(payload) != len(raw):
                raise ValueError('Replacement DDS size/header changed')
            if name == 'stay_tx_dialogue.dds' and not row['same_as_reviewed_original']:
                payload = transplant_variant(raw, payload)
                base = (bundle/'replacements/stay_tx_dialogue.dds').read_bytes()
                # Original reviewed base has exactly the same text ROI as this variant.
                original_base = Path('native/analysis/battle-ui-localization/stay_tx_dialogue.dds').read_bytes()
                reviewed_base = next(r for r in provenance if r['path'] == name)
                if ui.sha(original_base) != reviewed_base['sha256']:
                    raise ValueError('Reviewed base dialogue extraction changed')
                differences = sum(a != b for a, b in zip(raw, original_base))
                if differences != 53383:
                    raise ValueError('Variant differs from reviewed 53,383-byte non-ROI baseline')
                width = struct.unpack_from('<I', raw, 16)[0]
                x, y, w, h = ROI
                for yy in range(y, y+h):
                    at = 128+4*(yy*width+x)
                    if raw[at:at+4*w] != original_base[at:at+4*w]:
                        raise ValueError('Variant original text ROI differs')
                    if payload[at:at+4*w] != base[at:at+4*w]:
                        raise ValueError('Variant translated ROI differs')
                variant_proof = dict(original_sha256=ui.sha(raw), output_sha256=ui.sha(payload),
                                     preserved_nontranslation_differences=53383, roi=list(ROI),
                                     outside_roi_changed_bytes=0)
                replacement = Path(variant_output).resolve()
                if replacement.exists() and replacement.read_bytes() != payload:
                    raise FileExistsError('Variant output already exists with other contents')
                replacement.parent.mkdir(parents=True, exist_ok=True)
                replacement.write_bytes(payload)
            cursor = (cursor+ui.ALIGN-1)//ui.ALIGN*ui.ALIGN
            if cursor+len(payload) > 0xFFFFFFFF:
                raise ValueError('PIDX32 offset overflow')
            changes.append(dict(name=name, pack=m['pack'], replacement=str(replacement),
                                sha256=ui.sha(payload), offset=cursor, size=len(payload), compressed_size=0,
                                master_field=m['record']+12, embedded_field=e['record']+12,
                                source_tuple=row, master_record_sha256=ui.sha(master_blob[m['record']:m['record']+24]),
                                embedded_record_sha256=ui.sha(embedded_blob[e['record']:e['record']+24])))
            cursor += len(payload)
    if ui.sha(epoch_path.read_bytes()) != ui.sha(epoch_bytes):
        raise ValueError('Source epoch changed during planning')
    return dict(kind='eight explicit battle DATA alias records with individual appends',
                data2=str(source), index=str(index), pack_filename=source.name,
                source_data2_sha256=source_hash, source_pack_sha256=source_hash,
                source_index_sha256=index_hash, source_epoch_sha256=ui.sha(epoch_bytes),
                original_length=source.stat().st_size, final_length=cursor,
                changes=changes, variant_proof=variant_proof, source_modified=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, default=Path('localization/image_assets/battle-ui-v1'))
    parser.add_argument('--index', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    doc = plan(args.bundle, args.index or args.bundle/'staged-data2/pack.idx',
               args.bundle/'replacements/stay_tx_dialogue.variant.dds')
    if args.apply:
        if not args.output:
            parser.error('--apply requires --output NEW_DIRECTORY')
        doc = ui.apply(doc, args.output)
    print(json.dumps(doc, indent=2))


if __name__ == '__main__':
    main()
