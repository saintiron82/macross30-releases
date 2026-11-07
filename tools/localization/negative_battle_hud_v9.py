"""Negative fixtures for battle-hud-v9: the mask validator must reject tampered
candidates (protected SPA/digits/v8 特殊 pixels, pixels outside the label ROIs,
header bytes, wrong source SHA). Writes only into a temp directory."""
import json
import tempfile
from pathlib import Path

from stage_battle_hud_v9 import B, load, dump, validate_mask
from stage_ui_images import sha


def tampered(a, tmp, name, edit):
    raw = bytearray(Path(a['replacement']).read_bytes())
    edit(raw)
    p = Path(tmp) / f'{name}.dds'
    p.write_bytes(raw)
    return dict(a, replacement=str(p), replacement_sha256=sha(bytes(raw)))


def px(x, y):
    return 128 + (y * 1280 + x) * 4


def main():
    a = load(B / 'candidate/authority.json')
    validate_mask(a)
    cases = {
        'SPA_sprite_pixel': lambda r: r.__setitem__(px(1200, 615) + 3, r[px(1200, 615) + 3] ^ 0xFF),
        'digit_1_pixel': lambda r: r.__setitem__(px(1210, 650) + 3, r[px(1210, 650) + 3] ^ 0xFF),
        'v8_special_pixel': lambda r: r.__setitem__(px(865, 362) + 1, r[px(865, 362) + 1] ^ 0x10),
        'outside_roi_pixel': lambda r: r.__setitem__(px(100, 100) + 3, r[px(100, 100) + 3] ^ 0x01),
        'english_sublabel_pixel': lambda r: r.__setitem__(px(1210, 509) + 3, r[px(1210, 509) + 3] ^ 0x40),
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
