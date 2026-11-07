"""Read-only PES inventory; packet presence does not prove decoded audio."""
import argparse
import collections
import json
import mmap
from pathlib import Path


def inspect(path):
    counts = collections.Counter()
    examples = {}
    private_payloads = collections.Counter()
    private_streams = {}
    with path.open('rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
        if data[:4] != b'PAMF':
            raise ValueError('Expected PAMF')
        payload = int.from_bytes(data[8:12], 'big') * 2048
        pos = payload
        while True:
            pos = data.find(b'\x00\x00\x01', pos)
            if pos < 0 or pos + 6 > len(data):
                break
            kind = data[pos + 3]
            if kind >= 0xBB and kind != 0xFF:
                size = int.from_bytes(data[pos + 4:pos + 6], 'big')
                end = pos + 6 + size
                if size and end <= len(data):
                    key = f'{kind:02X}'
                    counts[key] += 1
                    examples.setdefault(key, dict(offset=pos, packet_bytes=size,
                                                 prefix=data[pos:pos + 32].hex()))
                    if kind == 0xBD and pos + 9 <= end:
                        start = pos + 9 + data[pos + 8]
                        if start < end:
                            private_payloads[data[start:start + 4].hex()] += 1
                            stream_id = f'{data[start]:02X}'
                            row = private_streams.setdefault(stream_id, dict(
                                packets=0, payload_bytes=0, first_offset=start,
                                first_payload_hex=data[start:min(end, start + 32)].hex()))
                            row['packets'] += 1
                            row['payload_bytes'] += end - start
                    pos = end
                    continue
            pos += 4
    return dict(source=str(path.resolve()), payload_offset=payload,
                packet_counts=dict(counts), first_packets=examples,
                private_payload_prefix_counts=dict(private_payloads.most_common(16)),
                private_substream_inventory=private_streams,
                limits=['Structural MPEG-PS PES inventory, not codec identification or PCM verification.',
                        'Private stream packets may require PS3-specific demuxing.'],
                source_modified=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('media', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = inspect(args.media)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result))
