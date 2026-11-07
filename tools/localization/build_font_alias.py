"""Append one ABF glyph alias without rasterizing or changing any DDS byte."""
import argparse
import json
from pathlib import Path
import struct

import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from audit_runtime_font import chunks, read_font
from stage_ui_images import sha, sha_file


def build(abf, dds, expected_abf, expected_dds, source=0x30FB, target=0xB7):
    if sha(abf) != expected_abf or sha(dds) != expected_dds:
        raise ValueError('Source font/DDS hash mismatch')
    common, glyphs, pages = read_font(abf)
    by_code = {g['codepoint']: g for g in glyphs}
    if len(by_code) != len(glyphs) or target in by_code or source not in by_code:
        raise ValueError('Source missing, target exists, or duplicate codepoint')
    donor = by_code[source]
    if donor['page'] >= common['page_count'] or donor['x']+donor['width'] > common['width'] or donor['y']+donor['height'] > common['height']:
        raise ValueError('Donor rectangle outside atlas')
    record = struct.pack('>I', target)+abf[donor['record_offset']+4:donor['record_offset']+32]
    output = bytearray()
    allowed = []
    insertion = None
    common_count = 0
    for off, magic, kind, payload, extra in chunks(abf):
        if (magic, kind) == (b'BFNT', b'HCMN'):
            common_count += 1
            payload = struct.pack('>I', common['glyph_count']+1)+payload[4:]
            allowed.append((off+16, 4))
        elif (magic, kind) == (b'BFNT', b'CBLK'):
            if insertion is not None:
                raise ValueError('Multiple CBLK chunks require separate review')
            old_size = len(payload)
            insertion = off+16+old_size
            payload += record
            allowed.append((off+8, 4))
            if extra == old_size+16:
                extra += 32
                allowed.append((off+12, 4))
            elif extra:
                raise ValueError('Unknown CBLK extra size convention')
        output.extend(struct.pack('>4s4sII', magic, kind, len(payload), extra)+payload)
    if common_count != 1 or insertion is None:
        raise ValueError('Expected one common header and one glyph table')
    nc, ng, np = read_font(output)
    if ng[:-1] != glyphs or np != pages or nc['glyph_count'] != common['glyph_count']+1:
        raise ValueError('Existing records/pages changed')
    if output[insertion:insertion+32] != record:
        raise ValueError('Alias insertion differs')
    reduced = output[:insertion]+output[insertion+32:]
    for at in range(len(abf)):
        if reduced[at] != abf[at] and not any(start <= at < start+size for start, size in allowed):
            raise ValueError('Unexpected ABF byte change')
    for glyph in glyphs:
        at = glyph['record_offset']
        if output[at:at+32] != abf[at:at+32]:
            raise ValueError('Existing glyph record changed')
    manifest = dict(source_abf_sha256=sha(abf), output_abf_sha256=sha(output),
                    source_dds_sha256=sha(dds), output_dds_sha256=sha(dds),
                    existing_records_identical=len(glyphs), glyph_count=nc['glyph_count'],
                    added_codepoint=f'U+{target:04X}', donor_codepoint=f'U+{source:04X}',
                    alias_record=ng[-1], insertion_offset=insertion, inserted_bytes=32,
                    allowed_header_fields=[dict(offset=a, bytes=n) for a, n in allowed],
                    all_other_ABF_bytes_identical=True, DDS_entire_file_unchanged=True,
                    in_game_verified=False, active_modified=False)
    repertoire = dict(glyph_count=len(ng), codepoints=sorted(g['codepoint'] for g in ng),
                      source_abf_sha256=sha(abf), output_abf_sha256=sha(output),
                      coverage_policy='Require every non-control Unicode scalar in strings to be present; no Hangul-only filter.')
    return bytes(output), manifest, repertoire


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--abf', type=Path, required=True)
    p.add_argument('--dds', type=Path, required=True)
    p.add_argument('--abf-sha256', required=True)
    p.add_argument('--dds-sha256', required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    result, manifest, repertoire = build(a.abf.read_bytes(), a.dds.read_bytes(), a.abf_sha256, a.dds_sha256)
    output = a.output.resolve()
    if output == a.abf.resolve() or output == a.dds.resolve() or output.exists():
        raise ValueError('Output must be a new independent ABF path')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(result)
    (output.parent/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    (output.parent/'glyph-repertoire.json').write_text(json.dumps(repertoire, indent=2)+'\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
