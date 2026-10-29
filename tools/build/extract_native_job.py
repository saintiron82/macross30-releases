"""Extract an observed raw SPURS job from an ELF PT_LOAD mapping."""
import hashlib
import json
from pathlib import Path
import struct

root = Path(__file__).resolve().parents[2]
elf = (root/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf').read_bytes()
address, size = 0xA62F00, 3472
phoff = struct.unpack_from('>Q', elf, 32)[0]
phsize, phnum = struct.unpack_from('>HH', elf, 54)
for i in range(phnum):
    kind, flags, offset, vaddr, _, filesz, memsz, align = struct.unpack_from('>IIQQQQQQ',elf,phoff+i*phsize)
    if kind == 1 and vaddr <= address and address+size <= vaddr+filesz:
        data = elf[offset+address-vaddr:offset+address-vaddr+size]
        break
else:
    raise SystemExit('Observed job is not backed by this ELF')
# Match the SDK's historical offset basis, which differs from standard FNV.
fingerprint = 1469598103934665603
for byte in data:
    fingerprint = ((fingerprint ^ byte)*0x100000001b3)&0xffffffffffffffff
assert fingerprint == 0x4C091E2C0BD5C570, hex(fingerprint)
out = root/'native/macross30/raw_jobs'
out.mkdir(exist_ok=True)
(out/'job_4c091e2c0bd5c570.bin').write_bytes(data)
manifest = dict(guest_ea=hex(address),size=size,fingerprint=hex(fingerprint),sha256=hashlib.sha256(data).hexdigest())
(out/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
print(json.dumps(manifest))
