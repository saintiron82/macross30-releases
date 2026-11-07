"""Restore the bounded caption-only gate after regenerating the PPU lift."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
path = ROOT/'native/macross30/recompiled/ppu_recomp_008.cpp'
text = path.read_text(encoding='utf8')
head = 'void func_003975D8(ppu_context* ctx) {\n'
gate = '        if (!macross_caption_visible()) return; // MACROSS_CAPTION_VISIBILITY\n'
assert text.count(head) == 1
body = text[text.index(head):text.index('\n}', text.index(head))]
for anchor in ('func_005BC190(ctx)', 'func_005BBEE8(ctx)', 'func_006894D0(ctx)', '0x100'):
    assert anchor in body, 'Caption draw contract changed: '+anchor
if gate not in body:
    text = text.replace(head, head+gate)
include = '#include "../subtitle_visibility.h"\n'
if include not in text:
    text = text.replace('#include "ppu_recomp.h"\n', '#include "ppu_recomp.h"\n'+include, 1)
if text != path.read_text(encoding='utf8'):
    path.write_bytes(text.encode('utf8'))
print('Caption draw gate present; update/voice functions untouched')
