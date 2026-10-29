"""Idempotent exact two-loop patches, guarded against original ELF codewords."""
from pathlib import Path
import struct
ROOT=Path(__file__).resolve().parents[2]
SPECS=[('ppu_recomp_003.cpp','001A279C','3','880300002f800000419efff84e800020'),('ppu_recomp_001.cpp','0009B220','31','881f00002f800000419efff8881f011b')]
def apply(root=ROOT):
 b=(root/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf').read_bytes();ph=struct.unpack_from('>Q',b,32)[0];esz,num=struct.unpack_from('>HH',b,54)
 for name,pc,reg,expected in SPECS:
  address=int(pc,16);matches=[]
  for i in range(num):
   typ,fl,off,va,pa,fs,ms,al=struct.unpack_from('>IIQQQQQQ',b,ph+i*esz)
   if typ==1 and va<=address and address+16<=va+fs:matches.append(b[off+address-va:off+address-va+16].hex())
  assert matches==[expected],(pc,matches)
  p=root/'native/macross30/recompiled'/name;s=p.read_text(encoding='utf-8');marker=f'/* MACROSS_WAIT_V2_{pc} */'
  if marker in s:continue
  legacy=f'/* MACROSS_WAIT_{pc} */'
  if legacy in s:
   s=s.replace(f'        uint32_t macross_wait_failed_{pc} = 0; {legacy}\n','')
   old_hook=f'        if (((ctx->cr >> 1) & 1)) {{ macross_ppu_wait_backoff(0x{pc}u, (uint32_t)ctx->gpr[{reg}], &macross_wait_failed_{pc}); goto loc_{pc}; }}'
   assert s.count(old_hook)==1
   s=s.replace(old_hook,f'        if (((ctx->cr >> 1) & 1)) goto loc_{pc};')
   s=s.replace('#include "../ppu_wait_backoff.h"\n','')
  old=f'        if (((ctx->cr >> 1) & 1)) goto loc_{pc};'
  assert s.count(old)==1,(name,s.count(old))
  # Counter lifetime is the original guest function invocation, including its original backedges.
  fn='func_001A279C' if reg=='3' else 'func_0009B1D8'
  begin=f'void {fn}(ppu_context* ctx) {{';assert s.count(begin)==1
  s=s.replace(begin,begin+f'\n        const bool macross_wait_on_{pc} = macross_ppu_wait_backoff_enabled()!=0;\n        uint32_t macross_wait_failed_{pc} = 0; {marker}',1)
  new=f'        if (((ctx->cr >> 1) & 1)) {{ if (macross_wait_on_{pc}) {{ ++macross_wait_failed_{pc}; if (macross_wait_failed_{pc} == 1 || (macross_wait_failed_{pc} & 4095u) == 0) macross_ppu_wait_backoff(0x{pc}u, (uint32_t)ctx->gpr[{reg}], macross_wait_failed_{pc}); }} goto loc_{pc}; }}'
  s=s.replace(old,new,1);s='#include "../ppu_wait_backoff.h"\n'+s;p.write_text(s,encoding='utf-8')
if __name__=='__main__':apply();print('Two exact wait loops patched (idempotent).')
