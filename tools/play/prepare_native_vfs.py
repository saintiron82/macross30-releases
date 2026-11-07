"""Prepare an unmodified Japanese disc baseline for native bring-up."""
import hashlib
import json
from pathlib import Path
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from inspect_iso import inventory, read_file


root = Path('native/vfs')
root.mkdir(parents=True, exist_ok=True)
marker = root / 'source_manifest.json'
if marker.exists():
    print('Native baseline already prepared:', root)
    raise SystemExit(0)
rows = []
with next(Path('.').glob('*/*.iso')).open('rb') as source:
    files = [r for r in inventory(source) if not r['path'].startswith('PS3_UPDATE/')]
    for i, row in enumerate(files):
        target = root / row['path']
        target.parent.mkdir(parents=True, exist_ok=True)
        digest = hashlib.sha256()
        with target.open('wb') as output:
            for offset in range(0, row['size'], 8*1024*1024):
                block = read_file(source, row, offset, min(8*1024*1024,row['size']-offset))
                if len(block) != min(8*1024*1024,row['size']-offset):
                    raise ValueError('Short ISO read')
                digest.update(block)
                output.write(block)
        assert target.stat().st_size == row['size']
        rows.append(dict(path=row['path'],size=row['size'],source_sha256=digest.hexdigest()))
        if i % 30 == 0:
            print('Native disc baseline:',i+1,'/',len(files),flush=True)
marker.write_text(json.dumps(dict(language='Japanese original',modified=False,files=rows),indent=2),encoding='utf-8')
print('Native baseline complete:',len(rows),'files',flush=True)
