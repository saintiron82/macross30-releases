"""Copy the ELF and translate reviewed UI strings without moving any bytes.

Only exact original NUL-terminated strings at reviewed file offsets are accepted.
Executable code, ELF headers and all bytes outside these string slots are retained.
"""
import argparse
import hashlib
import json
import struct
import re
from pathlib import Path
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from gop_strings import TOKEN

ROOT = Path(__file__).resolve().parents[2]
SLOTS = (
    (0x8F1068, '発進', '출격'),
    (0x93F958, 'クエスト詳細', '퀘스트 상세'),
    (0x93F970, 'クリア条件', '클리어조건'),
    (0x93F980, '報酬', '보상'),
    (0x9572B0, 'クエスト詳細', '퀘스트 상세'),
    (0x9572C8, 'クリア条件', '클리어조건'),
    (0x9572D8, '報酬', '보상'),
    (0x9584B0, '報酬', '보상'),
    (0x8FC120, 'マイシップ', '마이십'),
    (0x8FC130, 'ハンガー', '격납고'),
    (0x8FC140, 'セーブ・ロード', '저장/불러오기'),
    (0x8FC158, 'ギャラリー', '갤러리'),
    (0x8FC168, 'フィールド移動', '필드 이동'),
    (0x8FC180, '出撃', '출격'),
    (0x8FC188, '最終章に進む', '최종장 진행'),
    (0x8FC1A0, 'オプション', '옵션'),
    (0x8FC1B0, 'ユーリア群島', '유리아 군도'),
    (0x8FC1C8, 'シエラ砂漠', '시에라사막'),
    (0x8FC1D8, 'マディス氷河', '마디스 빙하'),
    (0x8FC1F0, 'セッティング', '설정'),
    (0x8FC208, 'セーブ', '저장'),
    (0x8FC218, 'ロード', '불러오기'),
    (0x8FC228, 'タイトルに戻る', '타이틀로 복귀'),
    (0x8FC240, '出撃しますか？', '출격할까요?'),
    (0x8FC258, '最終章に進んでもよろしいですか？', '최종장으로 진행할까요?'),
    (0x94C8F0, 'セーブ・ロード', '저장/불러오기'),
    (0x940650, 'オプション', '옵션'),
)
# LOAD is followed by six verified NUL padding bytes before the next reviewed string.
SLOT_BYTES = {0x8FC218: 16}


def font_codepoints(repertoire=None):
    if repertoire is not None:
        values = repertoire['codepoints']
        if not values or any(type(x) is not int or not 0 <= x <= 0x10FFFF for x in values):
            raise ValueError('Invalid font repertoire codepoints')
        return set(values)
    from audit_runtime_font import read_font
    _, original, _ = read_font((ROOT/'analysis/font_assets/font_j24x24.abf').read_bytes())
    points = {r['codepoint'] for r in original}
    points.update(r['codepoint'] for r in json.loads(
        (ROOT/'build/font-ko-full/glyph_metrics.json').read_text(encoding='utf-8-sig')))
    return points


def missing_characters(text, points):
    # printf substitution fields are not literal glyphs; CR/LF/TAB are layout.
    literal = re.sub(r'%[-+ #0]*\d*(?:\.\d+)?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcspn]', '', text)
    return sorted({ord(c) for c in literal if ord(c) >= 32 and ord(c) != 127 and ord(c) not in points})


def translate(source, names=None, ui_strings=None, font_repertoire=None):
    result = bytearray(source)
    changes = []
    allowed = set()
    glyphs = font_codepoints(font_repertoire)
    for offset, japanese, korean in SLOTS:
        original = japanese.encode('utf-8') + b'\0'
        replacement = korean.encode('utf-8') + b'\0'
        if source[offset:offset + len(original)] != original:
            raise ValueError(f'Original string mismatch at {offset:#x}')
        slot_bytes = SLOT_BYTES.get(offset, len(original))
        if source[offset:offset + slot_bytes] != original.ljust(slot_bytes, b'\0'):
            raise ValueError(f'Exact UI string/padding mismatch at {offset:#x}')
        if len(replacement) > slot_bytes:
            raise ValueError('Translation exceeds original string slot')
        if missing_characters(korean, glyphs):
            raise ValueError(f'UI font coverage missing at {offset:#x}')
        result[offset:offset + slot_bytes] = replacement.ljust(slot_bytes, b'\0')
        positions = set(range(offset, offset + slot_bytes))
        if allowed & positions:
            raise ValueError('Overlapping UI string slots')
        allowed.update(positions)
        changes.append(dict(file_offset=hex(offset), guest_address=hex(offset + 0x10000),
                            source=japanese, translation=korean, slot_bytes=slot_bytes, original_nul_string_bytes=len(original), verified_zero_padding=slot_bytes-len(original)))
    if ui_strings is not None:
        if not isinstance(ui_strings, (dict, list, tuple)):
            raise ValueError('UI strings must be a manifest or list of manifests')
        manifests = [ui_strings] if isinstance(ui_strings, dict) else ui_strings
        for manifest in manifests:
            if hashlib.sha256(source).hexdigest() != manifest['source_sha256']:
                raise ValueError('Basic UI source SHA256 mismatch')
            for row in manifest['strings']:
                offset = int(row['file_offset'], 16)
                original = row['source'].encode('utf-8') + b'\0'
                replacement = row['translation'].encode('utf-8') + b'\0'
                size = row['slot_bytes']
                if not len(original) <= size <= len(original) + 2 or offset < 0 or offset + size > len(source):
                    raise ValueError('Basic UI slot exceeds verified two-byte padding policy')
                slot = source[offset:offset + size]
                if slot != original.ljust(size, b'\0') or hashlib.sha256(slot).hexdigest() != row['original_slot_sha256']:
                    raise ValueError(f'Basic UI original string/padding/SHA mismatch at {offset:#x}')
                if len(replacement) > size:
                    raise ValueError('Basic UI translation exceeds slot')
                if TOKEN.findall(row['source']) != TOKEN.findall(row['translation']):
                    raise ValueError('Basic UI format tokens changed')
                if [c for c in row['source'] if c in '\r\n'] != [c for c in row['translation'] if c in '\r\n']:
                    raise ValueError('Basic UI newline sequence changed')
                if missing_characters(row['translation'], glyphs):
                    raise ValueError('Basic UI font coverage missing')
                positions = set(range(offset, offset + size))
                if allowed & positions:
                    raise ValueError('Overlapping basic UI translation slots')
                result[offset:offset + size] = replacement.ljust(size, b'\0')
                allowed.update(positions)
                changes.append(dict(file_offset=hex(offset), guest_address=hex(offset + 0x10000),
                                    source=row['source'], translation=row['translation'], slot_bytes=size,
                                    kind=manifest.get('kind', 'basic_ui'), translation_status=row['status']))
    if names is not None:
        if hashlib.sha256(source).hexdigest() != names['source_sha256']:
            raise ValueError('Fixed-name source SHA256 mismatch')
        if names['layout']['name_field_bytes'] != 64:
            raise ValueError('Unsupported fixed-name layout')
        for row in names['patch_candidates']:
            offset = int(row['file_offset'], 16)
            original = row['source'].encode('utf-8') + b'\0'
            replacement = row['translation'].encode('utf-8') + b'\0'
            slot = source[offset:offset + 64]
            if row['slot_bytes'] != 64 or len(original) > 64 or len(slot) != 64 or slot != original.ljust(64, b'\0'):
                raise ValueError(f'Fixed-name exact string/padding mismatch at {offset:#x}')
            if hashlib.sha256(slot).hexdigest() != row['original_slot_sha256']:
                raise ValueError(f'Fixed-name slot SHA256 mismatch at {offset:#x}')
            if len(replacement) > 64:
                raise ValueError('Fixed-name translation exceeds field')
            if missing_characters(row['translation'], glyphs):
                raise ValueError('Fixed-name font coverage missing')
            positions = set(range(offset, offset + 64))
            if allowed & positions:
                raise ValueError('Overlapping translation slots')
            result[offset:offset + 64] = replacement.ljust(64, b'\0')
            allowed.update(positions)
            changes.append(dict(file_offset=hex(offset), guest_address=row['guest_ea'],
                                source=row['source'], translation=row['translation'], slot_bytes=64,
                                kind='fixed_name', translation_status=row['translation_status']))
    changed = [i for i, (a, b) in enumerate(zip(source, result)) if a != b]
    if len(result) != len(source) or any(i not in allowed for i in changed):
        raise ValueError('Unexpected modification outside string slots')
    # PF_X PT_LOAD also contains read-only strings in this ELF; protect actual
    # executable sections and all header tables, not that whole mixed segment.
    if source[:6] != b'\x7fELF\x02\x02':
        raise ValueError('Expected big-endian ELF64')
    phoff, shoff = struct.unpack_from('>QQ', source, 32)
    ehsize, phsize, phnum, shsize, shnum = struct.unpack_from('>5H', source, 52)
    protected = [(0, ehsize), (phoff, phsize * phnum), (shoff, shsize * shnum)]
    for i in range(shnum):
        sh = shoff + i * shsize
        kind, flags = struct.unpack_from('>IQ', source, sh + 4)
        if kind == 1 and flags & 4:  # SHT_PROGBITS, SHF_EXECINSTR
            protected.append(struct.unpack_from('>QQ', source, sh + 24))
    if any(source[start:start + size] != result[start:start + size] for start, size in protected):
        raise ValueError('Executable code or ELF headers changed')
    return bytes(result), changes, len(changed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--replace', action='store_true', help='replace a previously generated output copy')
    parser.add_argument('--names', type=Path, help='Reviewed guarded fixed-name manifest')
    parser.add_argument('--ui-strings', type=Path, action='append', help='Reviewed exact-offset UI manifest; repeat to merge strictly guarded manifests')
    parser.add_argument('--font-repertoire', type=Path, help='Numeric codepoints from actual staged ABF')
    args = parser.parse_args()
    if (args.output.exists() and not args.replace) or args.source.resolve() == args.output.resolve():
        raise FileExistsError('Output must be a new independent ELF')
    original = args.source.read_bytes()
    names = json.loads(args.names.read_text(encoding='utf-8')) if args.names else None
    ui_strings = [json.loads(path.read_text(encoding='utf-8')) for path in args.ui_strings] if args.ui_strings else None
    repertoire = json.loads(args.font_repertoire.read_text(encoding='utf-8')) if args.font_repertoire else None
    result, changes, changed_bytes = translate(original, names, ui_strings, repertoire)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('wb' if args.replace else 'xb') as stream:
        stream.write(result)
    if args.output.read_bytes() != result:
        raise ValueError('ELF roundtrip mismatch')
    manifest = dict(source=str(args.source.resolve()), output=str(args.output.resolve()),
                    source_sha256=hashlib.sha256(original).hexdigest(),
                    output_sha256=hashlib.sha256(result).hexdigest(),
                    size=len(result), changed_bytes=changed_bytes,
                    outside_slot_changed_bytes=0, source_modified=False,
                    executable_sections_changed_bytes=0, elf_headers_changed_bytes=0,
                    names_manifest=str(args.names.resolve()) if args.names else None,
                    ui_strings_manifest=str(args.ui_strings[0].resolve()) if args.ui_strings else None,
                    ui_strings_manifests=[str(path.resolve()) for path in args.ui_strings] if args.ui_strings else [],
                    basic_ui_strings=sum(r.get('kind') == 'basic_ui' for r in changes),
                    menu_ui_strings=sum(r.get('kind') == 'menu_ui' for r in changes),
                    fixed_names=sum(r.get('kind') == 'fixed_name' for r in changes),
                    fixed_name_font_missing=0, ui_font_missing=0,
                    font_repertoire=str(args.font_repertoire.resolve()) if args.font_repertoire else 'original ABF + Korean metrics',
                    font_coverage='all literal Unicode; printf fields and CR/LF/TAB controls excluded',
                    reviewed_ui_slots=len(SLOTS), verified_padding_slots=len(SLOT_BYTES),
                    in_game_verified=False, strings=changes)
    args.output.with_suffix('.manifest.json').write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(manifest, ensure_ascii=True, indent=2))


if __name__ == '__main__':
    main()
