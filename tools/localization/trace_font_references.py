"""Find exact TOC pointer loads for font-related constants in the PPU ELF.

Only identifies pointer loads and nearest function descriptors. This does not
prove that a given routine renders the main dialogue font.
"""
import bisect
import json
import re
import struct
from pathlib import Path


elf_report = json.loads(Path('analysis/elf_analysis.json').read_text())
data = Path(elf_report['source']).read_bytes()
descriptors = json.loads(Path('analysis/ppu_function_descriptors.json').read_text())
starts = sorted(int(k) for k in descriptors)
shoff = struct.unpack_from('>Q', data, 40)[0]
shsize, shnum = struct.unpack_from('>HH', data, 58)
sections = [struct.unpack_from('>II4QII2Q', data, shoff + shsize * i) for i in range(shnum)]
executable = [r for r in sections if r[1] == 1 and r[2] & 4]
toc_values = {r['toc'] for records in descriptors.values() for r in records}
keys = [b'SpriteFontInitParams', b'SpriteFontContext', b'CharInfo', b'SpriteFontInfo',
        b'SINGLETON_SpriteFontMgr', b'SINGLETON_SpriteFontRenderMgr', b'FontCode',
        b'data/font/font_j24x24.abf']
records = []
for key in keys:
    location = data.index(key)
    pointers = [m.start() + 0x10000 for m in re.finditer(re.escape(struct.pack('>I', location + 0x10000)), data)]
    displacements = {pointer - toc for pointer in pointers for toc in toc_values if -32768 <= pointer - toc <= 32767}
    refs = []
    for section in executable:
        for pos in range(section[4], section[4] + section[5] - 3, 4):
            word = struct.unpack_from('>I', data, pos)[0]
            if word >> 26 != 32 or (word >> 16) & 31 != 2:
                continue
            displacement = word & 0xffff
            if displacement & 0x8000:
                displacement -= 65536
            if displacement not in displacements:
                continue
            address = section[3] + pos - section[4]
            index = bisect.bisect_right(starts, address) - 1
            refs.append(dict(instruction_vaddr=address, target_register=(word >> 21) & 31,
                             displacement=displacement, function_candidate=starts[index] if index >= 0 else None))
    records.append(dict(string=key.decode(), string_vaddr=location + 0x10000,
                        pointer_addresses=pointers, toc_loads=refs))
Path('analysis/font_references.json').write_text(json.dumps(dict(references=records,
    limitations='Exact lwz base-r2 displacement matches only. Static debug references do not prove rendering path.'), indent=2), encoding='utf-8')
for row in records:
    print(row['string'], [(hex(r['instruction_vaddr']), hex(r['function_candidate'])) for r in row['toc_loads']])
