"""Summarize recorded live RSX draws without treating submission as correct output."""
import argparse
import collections
import csv
import json
import re
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('csv', type=Path)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
with args.csv.open(encoding='utf-8') as stream:
    rows = list(csv.DictReader(line for line in stream if not line.startswith('#')))
targets = {}
for row in rows:
    target = targets.setdefault(row['surf'], dict(draws=0, formats=collections.Counter(),
        color_masks=collections.Counter(), texture_signatures=collections.Counter(),
        first_frame=int(row['frame']), last_frame=int(row['frame'])))
    target['draws'] += 1
    target['last_frame'] = int(row['frame'])
    signature = row.get('tex') or ''
    match = re.search(r' fmt=(\d+)', signature)
    target['formats'][match.group(1) if match else 'unknown'] += 1
    target['color_masks'][row['cmask']] += 1
    textures = signature.split(' bf=', 1)[0].strip()
    target['texture_signatures'][textures] += 1
for target in targets.values():
    target['texture_signatures'] = dict(target['texture_signatures'].most_common(16))
report = dict(source=str(args.csv.resolve()), rows=len(rows),
    interpretation='Recorded draws and guest state only; does not prove correct GPU pixels.',
    targets=targets)
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
print(f'{len(rows)} recorded draws across {len(targets)} targets: {args.output}')
