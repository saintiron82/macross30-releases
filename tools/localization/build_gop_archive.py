"""Build an independent FSTS patch artifact and verify every payload."""
import hashlib
import json
import struct
import zlib
from pathlib import Path
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from analyze_archives import cstring, unpack


original = Path('analysis/gop_archive.fsts').read_bytes()
patch_manifest = json.loads(Path('analysis/gop_validation.json').read_text())
approved = {row['file']: row for row in patch_manifest['patched']}
assert len(approved) == len(patch_manifest['patched'])
count, table, names, names_size = struct.unpack_from('<4I', original, 4)
entries = []
for i in range(count):
    no, off, size, zs = struct.unpack_from('<4I', original, table + 16 * i)
    name = cstring(original[names:names + names_size], no)
    payload = original[off:off + (zs if 0 < zs < size else size)]
    decoded = unpack(payload, size) if 0 < zs < size else payload
    entries.append((name, no, payload, decoded, zs))

first = min(struct.unpack_from('<I', original, table + 16 * i + 4)[0] for i in range(count))
output = bytearray(original[:first])
expected = []
for i, (name, no, payload, decoded, zs) in enumerate(entries):
    replacement = Path('build/patch') / Path(name).name
    if replacement.name in approved:
        decoded = replacement.read_bytes()
        assert hashlib.sha256(decoded).hexdigest() == approved[replacement.name]['sha256']
        payload = zlib.compress(decoded, 9)
        zs = len(payload)
    output.extend(b'\0' * (-len(output) % 16))
    off = len(output)
    output.extend(payload)
    struct.pack_into('<4I', output, table + 16 * i, no, off, len(decoded), zs)
    expected.append(decoded)

for i, decoded in enumerate(expected):
    no, off, size, zs = struct.unpack_from('<4I', output, table + 16 * i)
    payload = bytes(output[off:off + (zs if 0 < zs < size else size)])
    restored = unpack(payload, size) if 0 < zs < size else payload
    assert restored == decoded

target = Path('build/patch/gop_archive.fsts')
target.write_bytes(output)
manifest = dict(verified_payloads=count, original_size=len(original), rebuilt_size=len(output),
                growth=len(output)-len(original), sha256=hashlib.sha256(output).hexdigest(),
                applied_to_iso=False, runtime_verified=False,
                patched_files=sorted(approved),
                note='Update both outer PIDX sizes in an isolated game folder; runtime font validation remains required.')
Path('analysis/archive_validation.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
print(json.dumps(manifest, indent=2))
