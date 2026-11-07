"""Inventory PPU descriptors and direct branches relevant to text rendering.

This is bounded static analysis, not a disassembler or complete call graph.
Indirect calls and generated SPU jobs are deliberately not counted as resolved.
"""
import bisect
import json
import struct
from pathlib import Path


report = json.loads(Path('analysis/elf_analysis.json').read_text())
data = Path(report['source']).read_bytes()
shoff = struct.unpack_from('>Q', data, 40)[0]
shsize, shnum = struct.unpack_from('>HH', data, 58)
sections = []
for i in range(shnum):
    row = struct.unpack_from('>II4QII2Q', data, shoff + shsize * i)
    _, typ, flags, addr, off, size, *_ = row
    sections.append(dict(index=i, type=typ, flags=flags, vaddr=addr, offset=off, size=size))
executable = [s for s in sections if s['type'] == 1 and s['flags'] & 4]
entry = report['ppu']['entry']
opd = next(s for s in sections if s['vaddr'] <= entry < s['vaddr'] + s['size'])
assert opd['size'] % 8 == 0
functions = {}
for pos in range(opd['offset'], opd['offset'] + opd['size'], 8):
    code, toc = struct.unpack_from('>II', data, pos)
    assert any(s['vaddr'] <= code < s['vaddr'] + s['size'] for s in executable), hex(code)
    functions.setdefault(code, []).append(dict(descriptor=opd['vaddr'] + pos - opd['offset'], toc=toc))
starts = sorted(functions)
imports = {f['stub_vaddr']: dict(library=r['library'], name=f.get('name'), nid=f['nid'])
           for r in report['imports'] for f in r['functions']}
calls = []
direct_count = 0
for section in executable:
    for pos in range(section['offset'], section['offset'] + section['size'] - 3, 4):
        word = struct.unpack_from('>I', data, pos)[0]
        if word >> 26 != 18 or not word & 1:
            continue
        direct_count += 1
        address = section['vaddr'] + pos - section['offset']
        displacement = word & 0x03fffffc
        if displacement & 0x02000000:
            displacement -= 0x04000000
        target = displacement if word & 2 else address + displacement
        if target in imports:
            index = bisect.bisect_right(starts, address) - 1
            calls.append(dict(address=address, target=target,
                              enclosing_function_candidate=starts[index] if index >= 0 else None,
                              **imports[target]))
result = dict(descriptor_count=opd['size']//8, unique_descriptor_targets=len(functions),
              executable_bytes=sum(s['size'] for s in executable), direct_branch_link_instructions=direct_count,
              resolved_import_calls=calls, text_conversion_calls=[r for r in calls if r['library']=='cellL10n'],
              limitations='Only direct branch-and-link instructions. Nearest descriptor is a candidate boundary, not proven control flow.')
Path('analysis/ppu_calls.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
Path('analysis/ppu_function_descriptors.json').write_text(json.dumps(functions, indent=2), encoding='utf-8')
print({k:v for k,v in result.items() if k not in ('resolved_import_calls','text_conversion_calls')})
print('Direct import calls:',len(calls),'Text conversion calls:',len(result['text_conversion_calls']))
