"""Freeze an opt-in renderer experiment; preserve all shared native outputs."""
import hashlib
import json
from pathlib import Path
import shutil
import sys
import argparse

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/build'))
import build_subtitle_candidate as build


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--probe',action='store_true')
    parser.add_argument('--probe-revision',choices=['v2','v2-r2'],default='v2')
    parser.add_argument('--viewer',action='store_true')
    parser.add_argument('--battle-original',action='store_true');args=parser.parse_args()
    assert sum([args.viewer,args.probe,args.battle_original])<=1
    name='vf0-native1024-v4' if args.battle_original else 'vf0-hd-viewer-v3' if args.viewer else 'vf0-hd-'+args.probe_revision if args.probe else 'vf0-hd-v1'
    base=ROOT/'experiments'/name;base.mkdir(exist_ok=True)
    if args.battle_original:
        prior=ROOT/'experiments/vf0-hd-v1'
        shutil.copyfile(prior/'vf0_dds_guard.h',base/'vf0_dds_guard.h')
        hook=(prior/'vf0_hd_hook.h').read_text(encoding='utf8')
        hook=hook.replace('mb_00f-hd-2048.dds','gb_00f-native1024.dds').replace('vf0_layout.width==2048','vf0_layout.width==1024&&vf0_fnv(vf0_candidate,vf0_candidate_size)==UINT64_C(0xb73ebbc90fb3cce2)')
        hook=hook.replace('[vf0-hd]','[vf0-native1024]').replace('host 2048','host 1024 original gallery art').replace('512 -> 2048','512 -> 1024')
        (base/'vf0_hd_hook.h').write_text(hook,encoding='utf8')
        shutil.copyfile(ROOT/'analysis/vf0-hd-pilot/source/gb_00f_00.dds',base/'gb_00f-native1024.dds')
    if args.viewer:
        prior=ROOT/'experiments/vf0-hd-v1'
        data=(ROOT/'analysis/vf0-hd-pilot/source/gb_00f_00.dds').read_bytes()
        value=14695981039346656037
        for byte in data:value=((value^byte)*1099511628211)&((1<<64)-1)
        source_hash=f'0x{value:016x}'
        core=(prior/'vf0_dds_guard.h').read_text(encoding='utf8').replace('d.width!=512','d.width!=1024').replace('0x5814ec0c0d26f5f2',source_hash)
        (base/'vf0_dds_guard.h').write_text(core,encoding='utf8')
        hook=(prior/'vf0_hd_hook.h').read_text(encoding='utf8')
        hook=hook.replace('mb_00f-original.dds','gb_00f-original.dds').replace('mb_00f-hd-2048.dds','gb_00f-hd-2048.dds')
        hook=hook.replace('original.width==512','original.width==1024').replace('t->width!=512||t->height!=512','t->width!=1024||t->height!=1024')
        hook=hook.replace('0x5814ec0c0d26f5f2',source_hash).replace('131072','524288')
        hook=hook.replace('exact VF-0A BC1 source match only','exact VF-0A viewer BC1 source match only').replace('512 -> 2048','1024 -> 2048')
        (base/'vf0_hd_hook.h').write_text(hook,encoding='utf8')
        (base/'gb_00f-original.dds').write_bytes(data)
        shutil.copyfile(prior/'mb_00f-hd-2048.dds',base/'gb_00f-hd-2048.dds')
    if args.probe:
        prior=ROOT/'experiments/vf0-hd-v1'
        shutil.copyfile(prior/'vf0_dds_guard.h',base/'vf0_dds_guard.h')
        hook=(prior/'vf0_hd_hook.h').read_text(encoding='utf8')
        hook+=r'''
/* Capture bounded, actually sampled source data even on cache hits. No swap. */
static void vf0_probe_texture(const rsx_dsp_texture* t){
    InitOnceExecuteOnce(&vf0_once,vf0_init,NULL,NULL);
    if(!vf0_enabled||t->width<128||t->width>2048||t->height!=t->width||t->cubemap||t->dimension!=2)return;
    const u32 span=texture_source_span(t);if(!span||span>2*1024*1024)return;
    const u8* p=guest_ptr(t->location,t->offset,span);if(!p)return;
    const uint64_t stamp=vf0_fnv(p,span<256?span:256);
    static struct{u32 location,offset,format;uint64_t stamp;} seen[64];static u32 count;
    for(u32 i=0;i<count;i++)if(seen[i].location==t->location&&seen[i].offset==t->offset&&seen[i].format==t->format&&seen[i].stamp==stamp)return;
    if(count==64)return;
    seen[count].location=t->location;seen[count].offset=t->offset;seen[count].format=t->format;seen[count].stamp=stamp;count++;
    const char* root=getenv("MACROSS_VF0_HD_DIR");char path[4096];
    snprintf(path,sizeof(path),"%s/probe-%u-%08X-%02X-%016llX.bin",root,t->location,t->offset,t->format,(unsigned long long)stamp);
    FILE* f=fopen(path,"wb");if(f){fwrite(p,1,span,f);fclose(f);}
    fprintf(stderr,"[vf0-probe] location=%u offset=%08X fmt=%02X size=%ux%u dim=%u pitch=%u mips=%u span=%u first_mip_match=%d fnv=%016llX file=%s\n",t->location,t->offset,t->format,t->width,t->height,t->dimension,t->pitch,t->mipmaps,span,vf0_match_source(vf0_original,vf0_original_size,p,span),(unsigned long long)vf0_fnv(p,span),path);
}
'''
        (base/'vf0_hd_hook.h').write_text(hook,encoding='utf8')
    original=ROOT/'vendor/ps3recomp-master/libs/video/rsx_live_draw.c'
    text=original.read_text(encoding='utf8')
    needle='static ID3D12Resource* decode_guest_texture(const rsx_dsp_texture* t, u32 remap)\n{'
    assert text.count(needle)==1
    replacement='#include "'+str(base/'vf0_hd_hook.h').replace('\\','/')+'"\n'+needle+'\n    { ID3D12Resource* hd=vf0_try_texture(t); if(hd)return hd; }'
    altered=text.replace(needle,replacement)
    if args.probe:
        probe_needle='static u32 texture_srv_slot(const rsx_dsp_texture* t)\n{'
        assert altered.count(probe_needle)==1
        altered=altered.replace(probe_needle,probe_needle+'\n    vf0_probe_texture(t);')
    frozen=base/'rsx_live_draw_vf0.c';frozen.write_text(altered,encoding='utf8')
    out=ROOT/'native/subtitle-candidate'/name;out.mkdir(exist_ok=True)
    assert not (out/'build-result.json').exists()
    p=build.plan(out,[original],[base/'vf0_hd_hook.h',base/'vf0_dds_guard.h',frozen])
    row=p['compile'][0];row['source']=str(frozen);row['source_sha256']=build.sha(frozen)
    row['command'][-1]=str(frozen);row['command'].insert(-2,'/I'+str(original.parent))
    (out/'plan.json').write_text(json.dumps(p,indent=2),encoding='utf8')
    result=build.execute(p)
    (out/'build-result.json').write_text(json.dumps(result,indent=2),encoding='utf8')
    if not args.viewer:shutil.copyfile(ROOT/'analysis/vf0-hd-pilot/source/mb_00f.dds',base/'mb_00f-original.dds')
    print(json.dumps(dict(status=result['status'],candidate=str(out),normal_outputs_preserved=result['original_outputs_preserved'],enabled_by_default=False)))


if __name__=='__main__':main()
