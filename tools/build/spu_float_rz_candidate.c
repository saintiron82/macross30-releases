/* Offline only: guarded hardware RTZ experiment. Never linked into game. */
#include "spu_helpers.h"
#include <immintrin.h>
#pragma STDC FENV_ACCESS ON
static unsigned long long rz_fast[6],rz_fallback[6];
static int rz_plain(uint32_t u){unsigned e=(u>>23)&255;return (e>0&&e<255)||u==0;}
static u128 rz_reference(int op,u128 a,u128 b,u128 c){switch(op){case 0:return spu_fa_ref(a,b);case 1:return spu_fm_ref(a,b);case 2:return spu_fma_ref(a,b,c);case 3:return spu_fms_ref(a,b,c);case 4:return spu_fnms_ref(a,b,c);default:return spu_fs_ref(a,b);}}
static u128 rz_fallback_exact(int op,u128 a,u128 b,u128 c){switch(op){case 0:return spu__fa_s(a,b);case 1:return spu__fm_s(a,b);case 2:return spu__fma_s(a,b,c);case 3:return spu__fms_s(a,b,c);case 4:return spu__fnms_s(a,b,c);default:return spu__fs_s(a,b);}}
static inline u128 rz_candidate_impl(int op,u128 a,u128 b,u128 c,int scoped){
 unsigned original=scoped?0:_mm_getcsr(),rn=0x1f80u;int ok=1;
 for(int i=0;i<4;i++){
  if(!rz_plain(a._u32[i])||!rz_plain(b._u32[i])||(op>=2&&op<=4&&!rz_plain(c._u32[i])))ok=0;
  if((op==0||op==5)&&((a._u32[i]<<1)!=0)&&((b._u32[i]<<1)!=0)){int gap=(int)((a._u32[i]>>23)&255)-(int)((b._u32[i]>>23)&255);if(gap<=-30||gap>=30)ok=0;}
 }
 u128 out;
 if(ok){
  if(!scoped)_mm_setcsr(rn|0x6000u);
  __m128 x=_mm_loadu_ps((const float*)a._u32),y=_mm_loadu_ps((const float*)b._u32),z=_mm_loadu_ps((const float*)c._u32),r;
  switch(op){case 0:r=_mm_add_ps(x,y);break;case 1:r=_mm_mul_ps(x,y);break;case 2:r=_mm_fmadd_ps(x,y,z);break;case 3:r=_mm_fmsub_ps(x,y,z);break;case 4:r=_mm_fnmadd_ps(x,y,z);break;default:r=_mm_sub_ps(x,y);break;}
  _mm_storeu_ps((float*)out._u32,r);
  for(int i=0;i<4;i++){unsigned u=out._u32[i]&0x7fffffff,e=u>>23;if(e==0||e==255||u==0x7f7fffff)ok=0;}
 }
 if(ok)rz_fast[op]++;else{_mm_setcsr(rn);rz_fallback[op]++;out=rz_fallback_exact(op,a,b,c);if(scoped)_mm_setcsr(rn|0x6000u);}
 if(!scoped)_mm_setcsr(original);return out;
}

static inline u128 rz_candidate(int op,u128 a,u128 b,u128 c){return rz_candidate_impl(op,a,b,c,0);}
static inline u128 rz_candidate_scoped(int op,u128 a,u128 b,u128 c){return rz_candidate_impl(op,a,b,c,1);}
