"""Record glyph requirements and observed text-converter call arguments."""
import json
import struct
from pathlib import Path


chars = set()
translated = 0
for path in Path('localization/ko-KR/structured').glob('*.json'):
    for row in json.loads(path.read_text(encoding='utf-8'))['entries']:
        if row.get('translation'):
            translated += 1
            chars.update(row['translation'])
glyphs = []
for char in sorted(chars):
    encodings = {}
    for codec in ('utf-8', 'cp932', 'cp949'):
        try:
            encodings[codec] = char.encode(codec).hex()
        except UnicodeEncodeError:
            encodings[codec] = None
    glyphs.append(dict(character=char, codepoint=f'U+{ord(char):04X}', encodings=encodings))
elf = Path('analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf').read_bytes()
calls = json.loads(Path('analysis/ppu_calls.json').read_text())['text_conversion_calls']
observed = []
for call in calls:
    offset = call['address'] - 0x10000
    literals = []
    for pos in range(offset - 24, offset, 4):
        instruction = struct.unpack_from('>I', elf, pos)[0]
        opcode = instruction >> 26
        ra = (instruction >> 16) & 31
        rt = (instruction >> 21) & 31
        if opcode == 14 and ra == 0:  # addi rD,0,imm == li rD,imm
            value = instruction & 0xffff
            value = value - 65536 if value & 32768 else value
            literals.append(dict(vaddr=pos + 0x10000, register=rt, value=value))
    observed.append(dict(**call, nearby_li_instructions=literals))
result = dict(translated_entries=translated, unique_characters=len(glyphs),
              hangul_syllables=sum(0xac00<=ord(c)<=0xd7a3 for c in chars), glyphs=glyphs,
              unrepresentable_cp932=sum(r['encodings']['cp932'] is None for r in glyphs),
              converter_calls=observed,
              inference='Calls include literal code pages 0 (UTF-8), 3 (UCS-2), and 24 (CP932). '
                        'Static calls alone do not prove that GOP rendering uses this conversion path. '
                        'Do not change to CP949 without proving caller role and glyph mapping.',
              codepage_source='https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellL10n.h',
              font_rendering_verified=False)
Path('analysis/korean_encoding.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
print('Translated entries:', translated, 'Glyphs:',len(glyphs),'Hangul syllables:',result['hangul_syllables'],
      'Not encodable in CP932:',result['unrepresentable_cp932'])
