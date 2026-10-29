/* Diagnostic only: guest PUTs never dereference renderer-owned structures. */
#ifndef RSX_MAIN_ALIAS_AUDIT_H
#define RSX_MAIN_ALIAS_AUDIT_H
#define LD_MAA_EA 0x19700000u
#define LD_MAA_OFFSET 0x01400000u
#define LD_MAA_BYTES 0x00384000u
static volatile LONG s_maa_armed, s_maa_rows, s_maa_put_rows, s_maa_gpu_rows, s_maa_consumer_rows;
static volatile LONG s_maa_frame;
static volatile LONG64 s_maa_seq, s_maa_last_put;
static int s_maa_config=-1;
static u32 s_maa_last_frame=UINT32_MAX;
static const char* s_maa_gate;
static int s_maa_target_row=-1;
static u32 s_maa_put_base=LD_MAA_EA,s_maa_put_bytes=LD_MAA_BYTES;
static int ld_maa_overlap(u32 a,u64 bytes,u32 base,u64 length){
 return bytes&&(u64)a<(u64)base+length&&(u64)base<(u64)a+bytes;
}
static u64 ld_maa_next(void){return (u64)InterlockedIncrement64(&s_maa_seq);}
static void ld_maa_put_max(u64 value){
 LONG64 old=InterlockedCompareExchange64(&s_maa_last_put,0,0);
 while((u64)old<value){LONG64 seen=InterlockedCompareExchange64(&s_maa_last_put,(LONG64)value,old);if(seen==old)break;old=seen;}
}
/* Renderer owner only. No file I/O, getenv, clocks or renderer reads in SPU hook. */
static void ld_maa_owner_tick(void){
 if(s_maa_config<0){const char*e=getenv("LD_MAIN_ALIAS_AUDIT");s_maa_config=e&&!strcmp(e,"1");
  if(s_maa_config){s_maa_gate=getenv("LD_MAIN_ALIAS_AUDIT_GATE");
   e=getenv("LD_MAIN_ALIAS_AUDIT_ROW");if(e&&*e){char*end=NULL;long row=strtol(e,&end,10);
    if(end!=e&&!*end&&row>=0&&row<720){s_maa_target_row=(int)row;s_maa_put_base=LD_MAA_EA+(u32)row*5120u;s_maa_put_bytes=5120u;}}
  }}
 if(!s_maa_config||InterlockedCompareExchange(&s_maa_rows,0,0)>=64)return;
 InterlockedExchange(&s_maa_frame,(LONG)g_ld_frames);
 if(s_maa_last_frame==g_ld_frames)return;
 s_maa_last_frame=g_ld_frames;
 if((g_ld_frames&31u)!=0u)return;
 if(!InterlockedCompareExchange(&s_maa_armed,0,0)&&
    (!s_maa_gate||!s_maa_gate[0]||GetFileAttributesA(s_maa_gate)!=INVALID_FILE_ATTRIBUTES)){
  fprintf(stderr,"[main-alias-audit] armed frame=%u ea=%08X bytes=%u row_limit=64 put_limit=24 gpu_limit=16 consumer_limit=24 no_hash=1 no_readback=1 target_row=%d put_ea=%08X put_bytes=%u\n",g_ld_frames,LD_MAA_EA,LD_MAA_BYTES,s_maa_target_row,s_maa_put_base,s_maa_put_bytes);
  InterlockedExchange(&s_maa_armed,1);
 }
}
static int ld_maa_take(volatile LONG* category,LONG limit){
 if(InterlockedIncrement(category)>limit)return 0;
 if(InterlockedIncrement(&s_maa_rows)>64)return 0;
 return 1;
}
void rsx_live_draw_audit_guest_put(u32 ea,u32 bytes){
 if(!s_maa_armed||s_maa_rows>=64||!ld_maa_overlap(ea,bytes,s_maa_put_base,s_maa_put_bytes))return;
 u64 seq=ld_maa_next();ld_maa_put_max(seq);
 if(!ld_maa_take(&s_maa_put_rows,24))return;
 fprintf(stderr,"[main-alias-audit] seq=%llu event=spu_put_completed frame_hint=%ld host=%lu ea=%08X bytes=%u target_row=%d source=completed_guest_copy\n",(unsigned long long)seq,(long)InterlockedCompareExchange(&s_maa_frame,0,0),(unsigned long)GetCurrentThreadId(),ea,bytes,s_maa_target_row);
}
static void ld_maa_surface(u32 slot,const char* event,u32 kind,u32 src_abs,u32 src_pitch){
 ld_maa_owner_tick();
 if(!s_maa_armed||s_maa_rows>=64||slot>=g.n_surfaces)return;
 const surface_t*s=&g.surfaces[slot];
 u64 span=(u64)(s->pitch?s->pitch:s->w*4u)*s->h;
 if(s->location!=1||!ld_maa_overlap(s->offset,span,LD_MAA_OFFSET,LD_MAA_BYTES))return;
 u64 seq=ld_maa_next();int consumer=src_pitch!=0;
 if(!ld_maa_take(consumer?&s_maa_consumer_rows:&s_maa_gpu_rows,consumer?24:16))return;
 u32 serial=0,wgen=0,wkind=0,copygen=0,drawgen=0;
#if !defined(YZ_PERF_CLEAN)
 serial=s->resource_serial;wgen=s->last_write_generation;wkind=s->last_write_kind;copygen=s->last_copy_generation;drawgen=s->last_draw_generation;
#endif
 fprintf(stderr,"[main-alias-audit] seq=%llu event=%s frame=%u host=%lu slot=%u location=%u offset=%08X width=%u height=%u pitch=%u host_fmt=%u resource_serial=%u gpu_write_generation=%u gpu_write_kind=%u gpu_draw_generation=%u gpu_copy_generation=%u noted_kind=%u last_guest_put_seq=%llu src_abs=%08X src_pitch=%u timing=recording_not_GPU_completion\n",(unsigned long long)seq,event,g_ld_frames,(unsigned long)GetCurrentThreadId(),slot,s->location,s->offset,s->w,s->h,s->pitch,(u32)s->fmt,serial,wgen,wkind,drawgen,copygen,kind,(unsigned long long)InterlockedCompareExchange64(&s_maa_last_put,0,0),src_abs,src_pitch);
}
/* Physical MAIN row coordinates; box coordinates stay explicit for scaled /
 * reflected sampling. A halo is conservative provenance, not exact texel use. */
static int ld_maa_box_match(const surface_t*s,float x,float y,float w,float h,u32 halo){
 if(s->location!=1||(u32)s->fmt!=28u||!w||!h)return 0; /* audited RGBA8 byte contract */
 float x1=x+w,y1=y+h;if(x1<x){float t=x;x=x1;x1=t;}if(y1<y){float t=y;y=y1;y1=t;}
 x-=halo;y-=halo;x1+=halo;y1+=halo;
 if(x<0)x=0;if(y<0)y=0;if(x1>s->w)x1=(float)s->w;if(y1>s->h)y1=(float)s->h;
 if(x>=x1||y>=y1)return 0;
 u64 pitch=s->pitch?s->pitch:(u64)s->w*4u;
 u64 lo=(u64)s->offset+(u64)y*pitch+(u64)x*4u;
 u64 hi=(u64)s->offset+(u64)(y1-0.0001f)*pitch+(u64)(x1+0.9999f)*4u;
 u32 base=LD_MAA_OFFSET+(s_maa_target_row<0?0u:(u32)s_maa_target_row*5120u);
 u32 bytes=s_maa_target_row<0?LD_MAA_BYTES:5120u;
 return lo<(u64)base+bytes&&(u64)base<hi;
}
static void ld_maa_copy(u32 si,float sx,float sy,float sw,float sh,u32 di,u32 dx,u32 dy,u32 dw,u32 dh,const char* event,u32 halo){
 ld_maa_owner_tick();if(!s_maa_armed||s_maa_rows>=64||si>=g.n_surfaces||di>=g.n_surfaces)return;
 const surface_t*s=&g.surfaces[si],*d=&g.surfaces[di];
 int read=ld_maa_box_match(s,sx,sy,sw,sh,halo),write=ld_maa_box_match(d,(float)dx,(float)dy,(float)dw,(float)dh,0);
 if(!read&&!write)return;
 u64 seq=ld_maa_next();if(!ld_maa_take(&s_maa_consumer_rows,24))return;
 u32 sg=0,dg=0,sk=0,dk=0,ss=0,ds=0;
#if !defined(YZ_PERF_CLEAN)
 sg=s->last_write_generation;dg=d->last_write_generation;sk=s->last_write_kind;dk=d->last_write_kind;ss=s->resource_serial;ds=d->resource_serial;
#endif
 fprintf(stderr,"[main-alias-audit] seq=%llu event=%s frame=%u host=%lu target_row=%d main_read=%d main_write=%d src_slot=%u src_location=%u src_offset=%08X src_pitch=%u src_box=(%.4f,%.4f,%.4f,%.4f) src_fmt=%u src_serial=%u src_gpu_generation=%u src_gpu_kind=%u dst_slot=%u dst_location=%u dst_offset=%08X dst_pitch=%u dst_box=(%u,%u,%u,%u) dst_fmt=%u dst_serial=%u dst_gpu_generation_before_note=%u dst_gpu_kind_before_note=%u sample_halo=%u last_guest_put_seq=%llu timing=successful_GPU_command_record_not_completion\n",(unsigned long long)seq,event,g_ld_frames,(unsigned long)GetCurrentThreadId(),s_maa_target_row,read,write,si,s->location,s->offset,s->pitch?s->pitch:s->w*4u,sx,sy,sw,sh,(u32)s->fmt,ss,sg,sk,di,d->location,d->offset,d->pitch?d->pitch:d->w*4u,dx,dy,dw,dh,(u32)d->fmt,ds,dg,dk,halo,(unsigned long long)InterlockedCompareExchange64(&s_maa_last_put,0,0));
}

#endif
