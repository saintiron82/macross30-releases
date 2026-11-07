"""Compare an actual uploaded RGBA diagnostic against an original BGRA8 DDS."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('capture', type=Path, help='Diagnostic .ppm.json descriptor')
p.add_argument('original', type=Path)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
meta = json.loads(a.capture.read_text(encoding='utf-8'))
dds = a.original.read_bytes()
if len(dds) < 128 or dds[:4] != b'DDS ':
    raise ValueError('Invalid DDS header')
height, width, pitch = struct.unpack_from('<III', dds, 12)
header_flags = struct.unpack_from('<I', dds, 8)[0]
pf_size, flags, fourcc, bits, red, green, blue, alpha = struct.unpack_from('<8I', dds, 76)
if (pf_size, fourcc, bits, red, green, blue, alpha) != (
        32, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000):
    raise ValueError('Only original uncompressed BGRA8 DDS is supported')
expected_pitch = width * height * 4 if header_flags & 0x80000 else width * 4
if not flags & 0x40 or pitch not in (0, expected_pitch):
    raise ValueError('Unexpected original format or row pitch')
if (meta['width'], meta['height']) != (width, height):
    raise ValueError('Capture and original dimensions differ')
actual_path = a.capture.with_suffix('.rgba')
actual = actual_path.read_bytes()
size = width * height * 4
if len(actual) != size or len(dds) < 128 + size:
    raise ValueError('Incomplete pixel payload')
expected = bytearray(dds[128:128 + size])
expected[0::4], expected[2::4] = expected[2::4], expected[0::4]
different = sum(x != y for x, y in zip(actual, expected))
first = next((i for i, (x, y) in enumerate(zip(actual, expected)) if x != y), None)
result = dict(equal=different == 0, width=width, height=height,
              compared_bytes=size, different_bytes=different, first_different_byte=first,
              original=str(a.original), actual_upload=str(actual_path),
              original_sha256=hashlib.sha256(dds).hexdigest(),
              actual_sha256=hashlib.sha256(actual).hexdigest(),
              expected_rgba_sha256=hashlib.sha256(expected).hexdigest(),
              scope='Exact level0 uploaded pixels only; not full-frame rendering proof')
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(json.dumps(result, indent=2), encoding='utf-8')
print(json.dumps(result, indent=2))
raise SystemExit(0 if result['equal'] else 1)
