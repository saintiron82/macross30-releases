"""Inspect boot FSTS payloads for font resources and loader configuration."""
import json
import re
import struct
from pathlib import Path
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from inspect_iso import inventory, read_file
from analyze_archives import cstring, unpack


out = Path('analysis/boot_assets')
out.mkdir(parents=True, exist_ok=True)
archives = json.loads(Path('analysis/pack_archives.json').read_text())
report = []
with next(Path('.').glob('*/*.iso')).open('rb') as stream:
    files = inventory(stream)
    for archive in archives:
        if archive['path'] not in ('init', 'init_first', 'init_game'):
            continue
        pack = next(f for f in files if Path(f['path']).name.lower() == Path(archive['pack']).name.lower())
        blob = read_file(stream, pack, archive['offset'], archive['size'])
        assert blob[:4] == b'FSTS'
        num, table, names, names_size = struct.unpack_from('<4I', blob, 4)
        for i in range(num):
            no, off, size, zs = struct.unpack_from('<4I', blob, table + 16 * i)
            name = cstring(blob[names:names + names_size], no)
            payload = blob[off:off + (zs if 0 < zs < size else size)]
            data = unpack(payload, size) if 0 < zs < size else payload
            hits = [m.group().decode('ascii') for m in re.finditer(rb'[ -~]{5,}', data)
                    if any(k in m.group().lower() for k in (b'font', b'.ttf', b'.otf', b'.fnt', b'cellfont'))]
            signatures = {label: data.find(magic) for label, magic in
                          [('ttf', b'\x00\x01\x00\x00\x00'), ('otf', b'OTTO'), ('ttc', b'ttcf')]}
            row = dict(archive=archive['path'], path=name, size=len(data),
                       magic=data[:16].hex(), font_strings=hits, signatures=signatures)
            report.append(row)
            if hits or Path(name).suffix.lower() in ('.lua', '.txt', '.xml', '.ini', '.cfg'):
                target = out / archive['path'] / Path(name).name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
Path('analysis/boot_assets.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print('Boot payloads inspected:', len(report))
print(json.dumps([r for r in report if r['font_strings'] or r['signatures']['otf'] >= 0 or r['signatures']['ttc'] >= 0], indent=2))
