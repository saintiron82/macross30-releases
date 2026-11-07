"""Emit code identities without embedding any original instruction bytes."""
import hashlib
import re


def declaration(name, size, digest):
    if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', name):
        raise ValueError('Invalid signature name')
    if not isinstance(size, int) or size <= 0 or size > 0x40000:
        raise ValueError('Invalid SPU code length')
    if not re.fullmatch(r'[0-9a-f]{64}', digest):
        raise ValueError('Invalid SHA-256')
    values = ','.join('0x' + digest[i:i + 2] for i in range(0, 64, 2))
    return f'static const macross_code_signature {name} = {{{size},{{{values}}}}};\n'


def from_bytes(name, code):
    return declaration(name, len(code), hashlib.sha256(code).hexdigest())


def capture_header(rows):
    return ('/* Generated length/SHA-256 identities; no original code bytes. */\n'
            '#include "code_signature.h"\n' +
            ''.join(declaration(row['name'], row['size'], row['sha256'])
                    for row in rows))
