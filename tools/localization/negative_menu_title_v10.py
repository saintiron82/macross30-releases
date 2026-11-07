"""Negative fixtures for menu-title-v10: the mask validator must reject tampered candidates
(English sub-title pixels, the rule, a pixel outside the three title cells, header bytes, wrong
source SHA). Writes only into a temp directory."""
import json
import tempfile
from pathlib import Path

from stage_menu_title_v10 import B, load, dump, validate_mask
from stage_ui_images import sha


def tampered(a, tmp, name, edit):
    raw = bytearray(Path(a['replacement']).read_bytes())
    edit(raw)
    p = Path(tmp) / f'{name}.dds'
    p.write_bytes(raw)
    return dict(a, replacement=str(p), replacement_sha256=sha(bytes(raw)))


def px(x, y):
    return 128 + (y * 256 + x) * 4


def flip(x, y, ch, bit):
    return lambda r: r.__setitem__(px(x, y) + ch, r[px(x, y) + ch] ^ bit)


def main():
    a = load(B / 'candidate/authority.json')
    validate_mask(a)
    cases = {
        'difficulty_subtitle_pixel': flip(40, 125, 3, 0x40),
        'controller_subtitle_pixel': flip(180, 125, 3, 0x40),
        'rule_pixel': flip(100, 138, 3, 0x01),
        'outside_cells_pixel': flip(200, 230, 3, 0x01),
        'right_of_select_cell_pixel': flip(120, 170, 0, 0x10),
        'header_byte': lambda r: r.__setitem__(20, r[20] ^ 1),
    }
    results = {}
    with tempfile.TemporaryDirectory() as tmp:
        for name, edit in cases.items():
            try:
                validate_mask(tampered(a, tmp, name, edit))
                results[name] = 'ACCEPTED'
            except ValueError as e:
                results[name] = 'rejected: ' + str(e)
        try:
            validate_mask(dict(a, source_sha256='0' * 64))
            results['wrong_source_sha'] = 'ACCEPTED'
        except ValueError as e:
            results['wrong_source_sha'] = 'rejected: ' + str(e)
    ok = all(v.startswith('rejected') for v in results.values())
    dump(B / 'mask-negative-fixture.json', dict(status='PASS' if ok else 'FAIL', reviewed_candidate_accepted=True,
                                                cases=results))
    print(json.dumps(results, ensure_ascii=False, indent=1))
    if not ok:
        raise SystemExit('Negative fixture accepted a tampered candidate')


if __name__ == '__main__':
    main()
