"""Extract UI layout resources and inventory their font-related strings."""
import json
import re
from pathlib import Path
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from inspect_iso import inventory, read_file
from analyze_archives import unpack


rows = json.loads(Path('analysis/pack_inventory.json').read_text())
out = Path('analysis/ui_assets')
out.mkdir(parents=True, exist_ok=True)
report = []
with next(Path('.').glob('*/*.iso')).open('rb') as stream:
    packs = {Path(r['path']).name.lower(): r for r in inventory(stream)}
    for row in rows:
        if not row['path'].lower().endswith('.ark'):
            continue
        zs, size = row['compressed_size'], row['size']
        pack = packs[Path(row['pack']).name.lower()]
        data = read_file(stream, pack, row['offset'], zs if 0 < zs < size else size)
        if 0 < zs < size:
            data = unpack(data, size)
        target = out / Path(row['path']).name
        target.write_bytes(data)
        strings = [(m.start(), m.group().decode('ascii')) for m in re.finditer(rb'[ -~]{4,}', data)]
        hits = [(off, text) for off, text in strings if any(k in text.lower() for k in ('font', 'glyph', '.dds', 'charinfo'))]
        report.append(dict(path=row['path'], size=len(data), magic=data[:32].hex(), font_strings=hits))
Path('analysis/ui_assets.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print('UI resources inspected:', len(report))
print(json.dumps([r for r in report if r['font_strings']][:8], indent=2)[:6000])
