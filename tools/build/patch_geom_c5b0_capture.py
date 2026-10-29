"""Reproducible exact-source guarded C5B0 wrapper, no compilation/execution.
--check verifies deployed wrapper; --restore removes onlyourwrapper.
Originalwholeoracle can use original_source() afterinstrumentation.
"""
from pathlib import Path
import hashlib,argparse
R=Path(__file__).resolve().parents[2];P=R/'native/macross30/spu_gameplay/geom1/spu_recomp.c'
ORIGINAL_SHA='6325d3ab6816c893fa5f1471b7b6bd0651421cd035afca63633353aa8a182ea3'
INCLUDE='#include "../../geom_c5b0_capture.h"\n'
PUBLIC='void macross_geom1_spu_func_0000C5B0(spu_context* ctx) {'
PRIVATE='static void macross_geom1_c5b0_original_body(spu_context* ctx) {'
WRAPPER='''/* MACROSS_C5B0_CAPTURE_WRAPPER_BEGIN */
void macross_geom1_spu_func_0000C5B0(spu_context* ctx) {
    if (!g_macross_geom_c5b0_capture_on) { macross_geom1_c5b0_original_body(ctx); return; }
    const int token = macross_geom_c5b0_capture_pre(ctx);
    macross_geom1_c5b0_original_body(ctx);
    macross_geom_c5b0_capture_post(ctx, token);
}
/* MACROSS_C5B0_CAPTURE_WRAPPER_END */
'''
def original_source(s):
 if PRIVATE in s:
  assert s.count(WRAPPER)==1 and s.count(INCLUDE)==1
  s=s.replace(WRAPPER,'').replace(INCLUDE,'').replace(PRIVATE,PUBLIC,1)
 assert hashlib.sha256(s.encode()).hexdigest()==ORIGINAL_SHA,'originalgeomsourceSHAchanged'
 return s

def patched(s):
 s=original_source(s);a=s.index(PUBLIC);end=s.index('\nvoid ',a+1)
 return INCLUDE+s[:a]+s[a:end].replace(PUBLIC,PRIVATE,1)+WRAPPER+s[end:]
def contract():
 data=(R/'native/macross30/raw_jobs/job_3b9f76869d920608.bin').read_bytes()
 assert hashlib.sha256(data).hexdigest()=='9738c671984831af4ea6765bbc70583e00f5aca01313b96193bc74f04c6399e0'
 lines=['/* Generated exactoriginalcode; no modifiedguestbytes. */']
 for a,b in [(0xC5B0,0xD430),(0x88E0,0x88F8)]:
  raw=data[a:b];lines.append(f'static const unsigned char expected_{a:x}[{len(raw)}]={{')
  for i in range(0,len(raw),32):lines.append(','.join(f'0x{x:02x}'for x in raw[i:i+32])+',')
  lines.append('};')
 return '\n'.join(lines)+'\n'
if __name__=='__main__':
 ap=argparse.ArgumentParser();ap.add_argument('--check',action='store_true');ap.add_argument('--restore',action='store_true');a=ap.parse_args();s=P.read_text(encoding='utf-8-sig');new=original_source(s)if a.restore else patched(s);h=R/'native/macross30/geom_c5b0_capture_contract.h'
 if a.check:assert s==new and h.read_text()==contract()
 else:P.write_text(new,encoding='utf-8');h.write_text(contract(),encoding='utf-8')
 print('PASS exactsource wrapper',hashlib.sha256(new.encode()).hexdigest())
