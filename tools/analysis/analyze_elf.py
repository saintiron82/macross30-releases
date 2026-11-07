"""Inventory decrypted PPU ELF and validated embedded SPU ELF images."""
import hashlib
import json
import re
import struct
from pathlib import Path


def elf_header(blob, start=0):
    assert blob[start:start + 4] == b'\x7fELF'
    cls, endian = blob[start + 4:start + 6]
    assert endian == 2
    if cls == 2:
        values = struct.unpack_from('>HHIQQQIHHHHHH', blob, start + 16)
        fmt = '>II6Q'
    elif cls == 1:
        values = struct.unpack_from('>HHIIIIIHHHHHH', blob, start + 16)
        fmt = '>8I'
    else:
        raise ValueError('Unknown ELF class')
    kind, machine, version, entry, phoff, shoff, flags, hs, ps, pn, ss, sn, si = values
    assert hs in (52, 64) and ps == struct.calcsize(fmt)
    limit = max(hs, phoff + ps * pn, shoff + ss * sn)
    segments = []
    for i in range(pn):
        raw = struct.unpack_from(fmt, blob, start + phoff + ps * i)
        if cls == 2:
            typ, fl, off, va, pa, fs, ms, align = raw
        else:
            typ, off, va, pa, fs, ms, fl, align = raw
        assert off + fs <= len(blob) - start
        limit = max(limit, off + fs)
        segments.append(dict(type=typ, flags=fl, offset=off, vaddr=va, file_size=fs, memory_size=ms))
    for i in range(sn):
        row = struct.unpack_from('>II4QII2Q' if cls == 2 else '>10I', blob, start + shoff + ss * i)
        _, typ, _, _, off, size, *_ = row
        if typ != 8:
            limit = max(limit, off + size)
    assert start + limit <= len(blob)
    return dict(bits=64 if cls == 2 else 32, machine=machine, entry=entry,
                image_size=limit, program_headers=segments, section_count=sn)


source = Path('analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf')
data = source.read_bytes()
main = elf_header(data)
assert main['machine'] == 21


def virtual_offset(address, size=1):
    for segment in main['program_headers']:
        if segment['type'] == 1 and segment['vaddr'] <= address and address + size <= segment['vaddr'] + segment['file_size']:
            return segment['offset'] + address - segment['vaddr']
    raise ValueError(f'Unmapped address {address:x}')


param = next(p for p in main['program_headers'] if p['type'] == 0x60000002)
stub_start, stub_end = struct.unpack_from('>II', data, param['offset'] + 24)
imports = []
cursor = stub_start
while cursor < stub_end:
    off = virtual_offset(cursor, 44)
    length, _, version, attributes, functions, variables, tls = struct.unpack_from('>BB5H', data, off)
    assert length >= 44 and cursor + length <= stub_end
    name_addr, nid_addr, function_addr = struct.unpack_from('>3I', data, off + 16)
    no = virtual_offset(name_addr)
    name = data[no:data.index(b'\0', no)].decode('ascii')
    nids = struct.unpack_from(f'>{functions}I', data, virtual_offset(nid_addr, functions * 4))
    addresses = struct.unpack_from(f'>{functions}I', data, virtual_offset(function_addr, functions * 4))
    imports.append(dict(library=name, version=version, attributes=attributes, variables=variables, tls=tls,
                        functions=[dict(nid=f'0x{nid:08x}', stub_vaddr=address) for nid, address in zip(nids, addresses)]))
    cursor += length
assert cursor == stub_end
spus = []
out = Path('analysis/spu')
out.mkdir(parents=True, exist_ok=True)
for match in re.finditer(b'\x7fELF', data):
    if not match.start():
        continue
    hdr = elf_header(data, match.start())
    assert hdr['machine'] == 23
    payload = data[match.start():match.start() + hdr['image_size']]
    path = out / f'spu_{match.start():08x}.elf'
    path.write_bytes(payload)
    hdr.update(container_offset=match.start(), file=str(path), sha256=hashlib.sha256(payload).hexdigest())
    spus.append(hdr)
libraries = []
fonts = []
for match in re.finditer(rb'[ -~]{5,}', data):
    text = match.group().decode('ascii')
    if re.fullmatch(r'(?:cell|sys|sce)[A-Za-z0-9_]+', text):
        libraries.append(dict(offset=match.start(), value=text))
    if 'font' in text.lower():
        fonts.append(dict(offset=match.start(), value=text))
report = dict(source=str(source), sha256=hashlib.sha256(data).hexdigest(), ppu=main,
              embedded_spus=spus, library_name_string_candidates=libraries,
              font_string_candidates=fonts, imports=imports, import_table_parsed=True,
              nid_names_resolved=False, native_port_built=False)
Path('analysis/elf_analysis.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print('PPU bits:', main['bits'], 'SPU images:', len(spus), 'library strings:', len(libraries))
print('Font paths:', [r['value'] for r in fonts if '.abf' in r['value']])
print('Imported libraries:', [(r['library'], len(r['functions'])) for r in imports])
