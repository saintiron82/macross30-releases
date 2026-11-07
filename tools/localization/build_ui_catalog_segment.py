"""Offline reviewed display-table relocation. Never edits input or active files.

Uses an existing empty PT_LOAD descriptor; does not change native loader/code.
The original ELF supplies every literal/table-pointer guard. A separately
localized base may retain all earlier UI changes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import re

SHA = lambda b: hashlib.sha256(b).hexdigest()
PRINTF = re.compile(r'%%|%[-+ #0]*\d*(?:\.\d+)?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcspn]')


def headers(b):
    if b[:6] != b'\x7fELF\x02\x02':
        raise ValueError('ELF64 big-endian required')
    off = struct.unpack_from('>Q', b, 32)[0]
    size, count = struct.unpack_from('>HH', b, 54)
    if size != 56 or off + size * count > len(b):
        raise ValueError('invalid program headers')
    return off, size, [struct.unpack_from('>IIQQQQQQ', b, off+i*size) for i in range(count)]


def build(original, base, inventory, drafts, repertoire, address=0xDA0000):
    if SHA(original) != inventory['source_sha256']:
        raise ValueError('original SHA mismatch')
    po, size, ph = headers(original)
    if len(base) != len(original) or base[:po+len(ph)*size] != original[:po+len(ph)*size]:
        raise ValueError('base header/length mismatch')
    slot = 2
    if ph[slot] != (1, 4, 0xB754B4, 0, 0, 0, 0, 0x10000):
        raise ValueError('empty read-only PT_LOAD guard failed')
    if address != 0xDA0000:
        raise ValueError('only reviewed address DA0000 admitted')
    entries = {r['file_offset'].lower(): r for r in inventory['entries']}
    targets = {}
    for r in inventory['entries']:
        if r.get('translation_draft') and not r.get('installation_excluded', False):
            targets[r['file_offset'].lower()] = r['translation_draft']
    for draft in drafts:
        for r in draft['rows']:
            key = r['file_offset'].lower()
            if key not in entries or r['source'] != entries[key]['source']:
                raise ValueError('draft source mismatch')
            if entries[key].get('installation_excluded', False):
                continue
            targets[key] = r['target']
    payload = bytearray()
    edits, rows = {}, []
    for key, text in targets.items():
        r = entries[key]
        if r['classification'] == 'English_or_symbols_preserve':
            raise ValueError('English preservation violated')
        old = r['source'].encode()+b'\0'
        f = int(key, 16)
        if original[f:f+len(old)] != old:
            raise ValueError('literal guard failed')
        if text.count('\n') != r['source'].count('\n') or '\0' in text:
            raise ValueError('newline/NUL guard failed')
        if PRINTF.findall(text) != PRINTF.findall(r['source']):
            raise ValueError('printf format guard failed')
        missing = sorted({ord(c) for c in text if c not in '\r\n\t' and ord(c) not in repertoire})
        if missing:
            raise ValueError('missing font glyphs: '+str(missing))
        while len(payload) % 4:
            payload.append(0)
        target = address+len(payload)
        payload.extend(text.encode()+b'\0')
        refs = sorted({int(a['pointer_file_offset'], 16) for a in r['refs']})
        if not refs:
            raise ValueError('no reviewed display references')
        for ref in refs:
            expected = int(r['guest_ea'], 16)
            if ref % 4 or struct.unpack_from('>I', original, ref)[0] != expected or base[ref:ref+4] != original[ref:ref+4]:
                raise ValueError('typed pointer guard failed')
            if ref in edits and edits[ref] != target:
                raise ValueError('conflicting pointer writes')
            edits[ref] = target
        rows.append(dict(source_file_offset=key, source=r['source'], target=text,
                         target_guest_ea=hex(target), pointer_file_offsets=[hex(x) for x in refs]))
    if not payload:
        raise ValueError('empty payload')
    end = address+len(payload)
    if end > 0xE00000:
        raise ValueError('reviewed low-RAM arena capacity exceeded')
    for t, flags, off, va, pa, fs, ms, align in ph:
        if t == 1 and ms and address < va+ms and va < end:
            raise ValueError('PT_LOAD overlap including BSS')
    fileoff = (len(base)+65535) & ~65535
    out = bytearray(base)
    out.extend(bytes(fileoff-len(out)))
    out.extend(payload)
    struct.pack_into('>IIQQQQQQ', out, po+slot*size, 1, 4, fileoff, address, address, len(payload), len(payload), 65536)
    for ref, target in edits.items():
        struct.pack_into('>I', out, ref, target)
    allowed = set(range(po+slot*size, po+(slot+1)*size))
    for ref in edits:
        allowed.update(range(ref, ref+4))
    changed = {i for i, (a,b) in enumerate(zip(base, out)) if a != b}
    if changed-allowed:
        raise AssertionError('out-of-scope byte change')
    for row in rows:
        start=fileoff+int(row['target_guest_ea'],16)-address
        raw=row['target'].encode()+b'\0'
        assert out[start:start+len(raw)] == raw
    return bytes(out), dict(status='offline candidate; not installed or game-tested',
        game_ready=False, guest_private_heap_nonoverlap_verified=False,
        admission_blocker='DA0000 is outside original image/BSS and runtime reservations, but private guest malloc/CRT heap ownership is not yet established. Do not install.',
        original_sha256=SHA(original), base_sha256=SHA(base), output_sha256=SHA(out),
        segment_index=slot, segment_file_offset=hex(fileoff), segment_guest_range=[hex(address),hex(end)],
        segment_bytes=len(payload), translated_literals=len(rows), typed_pointer_count=len(edits),
        original_prefix_changes=len(changed), outside_allowed_changes=0,
        existing_load_headers_preserved=True, original_BSS_bounds_preserved=True,
        original_literals_preserved=True, prior_base_UI_bytes_outside_typed_pointers_preserved=True,
        printf_format_tokens_preserved=True, real_newline_counts_preserved=True,
        unreviewed_literal_references='left unchanged; no blind pointer replacement', rows=rows)


def main():
    a=argparse.ArgumentParser(description=__doc__)
    for name in ('original','base','inventory','font-repertoire','output'):
        a.add_argument('--'+name, required=True, type=Path)
    a.add_argument('--draft', action='append', default=[], type=Path)
    n=a.parse_args()
    load=lambda p: json.loads(p.read_text(encoding='utf-8-sig'))
    out, proof=build(n.original.read_bytes(),n.base.read_bytes(),load(n.inventory),[load(p) for p in n.draft],set(load(n.font_repertoire)['codepoints']))
    if n.output.exists():
        raise SystemExit('output already exists; choose a new offline candidate path')
    n.output.parent.mkdir(parents=True,exist_ok=True)
    n.output.write_bytes(out)
    n.output.with_suffix('.validation.json').write_text(json.dumps(proof,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
    print('PASS',proof['translated_literals'],'literals;',proof['typed_pointer_count'],'typed pointers;',proof['output_sha256'])


if __name__ == '__main__':
    main()
