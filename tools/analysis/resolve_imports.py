"""Match imported NIDs to symbol strings in the user's RPCS3 executable.

NID definition reference: RPCS3 Emu/Cell/PPUModule.cpp, ppu_generate_id.
Names are accepted only when their full salted SHA-1 NID matches an import.
"""
import hashlib
import json
import re
from pathlib import Path


suffix = bytes.fromhex('6759659904250490566427499489741a')
exe = Path('runtime/rpcs3/rpcs3.exe').read_bytes()
candidates = set(m.group().decode('ascii') for m in re.finditer(rb'[A-Za-z_][A-Za-z0-9_]{2,150}', exe))
lookup = {}
for name in candidates:
    nid = int.from_bytes(hashlib.sha1(name.encode('ascii') + suffix).digest()[:4], 'little')
    lookup.setdefault(f'0x{nid:08x}', []).append(name)
report = json.loads(Path('analysis/elf_analysis.json').read_text())
count = 0
unresolved = []
for library in report['imports']:
    for function in library['functions']:
        names = sorted(lookup.get(function['nid'], []))
        if len(names) == 1:
            function['name'] = names[0]
            function['name_evidence'] = 'RPCS3 executable symbol string with exact NID hash match'
            count += 1
        else:
            function['name_candidates'] = names
            unresolved.append(dict(library=library['library'], nid=function['nid'], candidates=names))
report['nid_names_resolved'] = not unresolved
report['resolved_import_count'] = count
report['unresolved_imports'] = unresolved
Path('analysis/elf_analysis.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print('Resolved:', count, 'Unresolved:', len(unresolved))
print(json.dumps(unresolved, indent=2))
print('Text conversion imports:', [f.get('name') for r in report['imports'] if r['library'] == 'cellL10n' for f in r['functions']])
