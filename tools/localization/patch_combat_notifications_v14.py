"""Prepare a separate, exact-slot combat notification ELF; never select/install it."""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
BUNDLE = ROOT / 'localization/image_assets/battle-voice-font-v13'
SOURCE_SHA = '3311e1c90d8d61da63e4f66437b6a3fbff4bb86781171da8703498e5bd237ae6'
FONT_SHA = '5cedde6325a9098d4d3c57dcc0bb9a62e44dade06476deb6a41973111067b487'
FORMAT = re.compile(r'%%|%[-+ #0]*\d*(?:\.\d+)?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcspn]')
SLOTS = (
    (0x8EE2F8, '%sのレベルが%dにあがった！', '%s 레벨이 %d로 상승!'),
    (0x8EE328, '%sをおぼえた！', '%s 습득!'),
    (0x8EE340, '%sのパラメーターがあがった！', '%s 능력치 상승!'),
    (0x8EE370, '体力', '체력'),
    (0x8EE378, '射撃', '사격'),
    (0x8EE380, '格闘', '격투'),
    (0x8EE388, '防御', '방어'),
    (0x8EE390, '速度', '속도'),
    (0x8EE398, '集中', '집중'),
    (0x8EE3A0, '魂', '혼'),
    (0x8EE3E0, '%sを撃破！　%4dTP、%4dEXP取得！', '%s 격파! %4dTP, %4dEXP 획득!'),
    (0x8EE410, '%4dTP、%4dEXP取得！', '%4dTP, %4dEXP 획득!'),
    (0x938AE0, '敵を撃破し、障壁が消滅した', '적 격파로 장벽이 사라졌다.'),
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def tokens(text):
    if '%' in FORMAT.sub('', text):
        raise ValueError('Unsupported or incomplete printf token')
    return FORMAT.findall(text)


def transform(source, slots, codepoints, expected_sha=SOURCE_SHA):
    if sha(source) != expected_sha:
        raise ValueError('Pinned source SHA mismatch')
    result = bytearray(source)
    ranges = []
    changes = []
    for offset, original, translation in slots:
        before = original.encode('utf8') + b'\0'
        after = translation.encode('utf8') + b'\0'
        if source[offset:offset + len(before)] != before:
            raise ValueError(f'Exact original NUL slot mismatch at {offset:#x}')
        if '\0' in translation or len(after) > len(before):
            raise ValueError('Translation exceeds original NUL slot')
        if tokens(original) != tokens(translation):
            raise ValueError('Printf token identity/order/width drift')
        literal = FORMAT.sub('', translation)
        missing = {ord(c) for c in literal if c not in '\r\n\t' and ord(c) not in codepoints}
        if missing:
            raise ValueError(f'Missing font codepoints: {sorted(missing)}')
        end = offset + len(before)
        if any(offset < previous_end and previous_start < end for previous_start, previous_end in ranges):
            raise ValueError('Overlapping slots')
        ranges.append((offset, end))
        result[offset:end] = after.ljust(len(before), b'\0')
        changes.append(dict(file_offset=hex(offset), source=original, translation=translation,
            original_UTF8_bytes=len(before)-1, replacement_UTF8_bytes=len(after)-1,
            slot_bytes_including_NUL=len(before), zero_padding_bytes=len(before)-len(after),
            printf_tokens=tokens(original)))
    result = bytes(result)
    if len(source) != len(result):
        raise ValueError('ELF size changed')
    cursor = 0
    for start, end in sorted(ranges):
        if source[cursor:start] != result[cursor:start]:
            raise ValueError('Bytes outside slots changed')
        cursor = end
    if source[cursor:] != result[cursor:]:
        raise ValueError('Tail outside slots changed')
    return result, changes


def self_tests():
    points = set(range(32, 127)) | {ord('가')}
    fixture = b'HEAD' + '%s xxx %4d'.encode() + b'\0TAIL'
    fixture_sha = sha(fixture)
    valid = ((4, '%s xxx %4d', '%s 가 %4d'),)
    result, _ = transform(fixture, valid, points, fixture_sha)
    assert result[:4] == b'HEAD' and result[-4:] == b'TAIL' and len(result) == len(fixture)
    cases = [
        ('bad_source', lambda: transform(fixture+b'!', valid, points, fixture_sha)),
        ('overflow', lambda: transform(b'%s\0', ((0, '%s', '%s가'),), points, sha(b'%s\0'))),
        ('placeholder_order', lambda: transform(fixture, ((4, '%s xxx %4d', '%4d %s'),), points, fixture_sha)),
        ('placeholder_width', lambda: transform(fixture, ((4, '%s xxx %4d', '%s %d'),), points, fixture_sha)),
        ('original_slot', lambda: transform(fixture, ((4, '%s bad %4d', '%s %4d'),), points, fixture_sha)),
    ]
    for name, action in cases:
        try:
            action()
        except ValueError:
            continue
        raise AssertionError(f'Negative fixture accepted: {name}')
    return dict(status='PASS', positive_in_place_fixture=True, rejected=[name for name, _ in cases])


def write_new_or_identical(path, data, approved_previous_sha=None):
    if path.exists():
        if path.read_bytes() != data:
            if sha(path.read_bytes()) != approved_previous_sha:
                raise ValueError(f'Refuse to overwrite different output: {path}')
            # Only the two exact unpublished predecessor artifacts are approved.
            path.write_bytes(data)
    else:
        with path.open('xb') as file:
            file.write(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true', help='In-memory fixtures only; write nothing')
    args = parser.parse_args()
    tests = self_tests()
    if args.self_test:
        print(json.dumps(tests))
        return
    ready_path = BUNDLE/'ready.json'
    ready_bytes = ready_path.read_bytes()
    ready = json.loads(ready_bytes)
    source_path = BUNDLE/ready['elf']['path']
    source = source_path.read_bytes()
    if sha(source) != ready['elf']['sha256'] or sha(source) != SOURCE_SHA:
        raise ValueError('Ready ELF and pinned source differ')
    barrier = SLOTS[-1][1].encode('utf8') + b'\0'
    if source.find(barrier) != 0x938AE0:
        raise ValueError('Barrier literal found at unexpected offset')
    authority = json.loads((BUNDLE/'candidate/authority.json').read_bytes())
    font = next(row for row in authority['entries'] if row['type'] == 'ABF')
    font_path = Path(font['replacement'])
    font_data = font_path.read_bytes()
    if sha(font_data) != FONT_SHA or font['replacement_sha256'] != FONT_SHA:
        raise ValueError('Required font SHA mismatch')
    from audit_runtime_font import read_font
    _, glyphs, _ = read_font(font_data)
    codepoints = {row['codepoint'] for row in glyphs}
    if len(codepoints) != 8177:
        raise ValueError('Required font glyph count changed')
    held=[]
    selected=[]
    for slot in SLOTS:
        offset,original,translation=slot
        if offset==0x938AE0 and len(translation.encode('utf8'))>len(original.encode('utf8')):
            held.append(dict(file_offset=hex(offset),source=original,translation=translation,
                original_UTF8_bytes=len(original.encode('utf8')),replacement_UTF8_bytes=len(translation.encode('utf8')),
                reason='Requested barrier translation exceeds original strlen by one byte; slot preserved unchanged.'))
        else:
            selected.append(slot)
    result, changes = transform(source, selected, codepoints)
    output = BUNDLE/'EBOOT.combat-notifications-v14.elf'
    manifest_path = BUNDLE/'combat-notifications-v14.manifest.json'
    manifest = dict(status='offline candidate; not selected or game-rendered',
        source_path=str(source_path),source_sha256=SOURCE_SHA,ready_sha256=sha(ready_bytes),
        output_path=str(output),output_sha256=sha(result),file_bytes=len(result),
        entries=len(changes),reviewed_entries=len(SLOTS),held_entries=held,
        maximum_replacement_UTF8_bytes=max(r['replacement_UTF8_bytes'] for r in changes),
        font_ABF_sha256=FONT_SHA,font_codepoints=8177,font_missing_codepoints=[],
        byte_changes_outside_reviewed_slots=0,file_size_unchanged=True,
        original_strlen_NUL_slots_only=True,printf_identity_order_width_preserved=True,
        ready_and_source_preserved=True,selected=False,game_started=False,
        self_tests=tests,changes=changes)
    write_new_or_identical(output,result,'3a873be01c65bde28b8ee099a5517640f8bdfaeacb6c9110c6612623b5acc3c4')
    if sha(output.read_bytes()) != manifest['output_sha256']:
        raise ValueError('Output readback failed')
    if ready_path.read_bytes() != ready_bytes or source_path.read_bytes() != source:
        raise ValueError('Original source or ready JSON changed')
    write_new_or_identical(manifest_path,(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n').encode('utf8'),
        '51e9b2bf94c5a86310751eb1e280e627c6253a14e2cdd2c2b818e70a7587ec48')
    print(json.dumps({k:v for k,v in manifest.items() if k!='changes'},ensure_ascii=False,indent=2))


if __name__ == '__main__':
    main()
