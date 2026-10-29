"""Bounded local ELF patching. Never accepts a downloaded full executable."""
import hashlib
import io
from pathlib import Path
import struct
import zlib


def apply(source, delta):
    if source[:4] != b'\x7fELF' or delta[:8] != b'MKDELTA1':
        raise ValueError('Original ELF and MKDELTA1 patch required')
    stream = io.BytesIO(delta)
    stream.seek(8)
    version, block = struct.unpack('<II', stream.read(8))
    size, = struct.unpack('<Q', stream.read(8))
    digest = stream.read(32)
    target_size, = struct.unpack('<Q', stream.read(8))
    target_digest = stream.read(32)
    count, = struct.unpack('<I', stream.read(4))
    if version != 1 or size != len(source) or hashlib.sha256(source).digest() != digest or target_size > 32 * 1024 * 1024 or count > 100000:
        raise ValueError('Original ELF identity or patch bounds mismatch')
    ops = [struct.unpack('<BQQ', stream.read(17)) for _ in range(count)]
    literal_size, = struct.unpack('<Q', stream.read(8))
    packed = stream.read()
    if len(packed) != literal_size:
        raise ValueError('Truncated patch')
    decoder = zlib.decompressobj(-15)
    literal = decoder.decompress(packed, target_size + 1)
    if not decoder.eof or len(literal) > target_size:
        raise ValueError('Invalid or oversized patch literals')
    output = bytearray()
    cursor = 0
    for kind, offset, length in ops:
        if len(output) + length > target_size:
            raise ValueError('Output bound exceeded')
        if kind == 0 and offset + length <= len(source):
            output.extend(source[offset:offset + length])
        elif kind == 1 and cursor + length <= len(literal):
            output.extend(literal[cursor:cursor + length])
            cursor += length
        else:
            raise ValueError('Invalid patch operation')
    if len(output) != target_size or cursor != len(literal) or hashlib.sha256(output).digest() != target_digest:
        raise ValueError('Generated ELF hash mismatch')
    return bytes(output)
