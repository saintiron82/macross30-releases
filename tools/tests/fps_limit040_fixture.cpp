#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <initializer_list>
#include "frame_pacing.cpp"
static bool gate_ok=true, measured=true;
extern "C" uint32_t vm_read32(uint64_t address) {
    if (!gate_ok) return 0;
    switch(address) {
    case 0x005590D0u:return 0x912B0178u;
    case 0x005590D4u:return 0x800B0178u;
    case 0x005590D8u:return 0x2B800001u;
    case 0x005590DCu:return 0x409D000Cu;
    default:assert(false);return 0;
    }
}
extern "C" bool macross_runtime_has_measured_step(void) {return measured;}
int main() {
    _putenv_s("MACROSS_DT_STEP","1");
    for(int limit:{30,60,0,60,30}) {
        int mode=macross_fps_limit_to_mode(limit);
        assert(macross_runtime_set_fps_unlocked(mode)==mode);
        assert(macross_fps_mode_to_limit(mode)==limit);
        unsigned count=macross_runtime_frame_vblanks();
        assert(count==(limit==30?2u:limit==60?1u:0u));
        if(count) {
            unsigned counter=0,frames=0;
            // The accepted main publishes at 60 Hz; use its actual gate threshold.
            for(unsigned beat=0;beat<120;beat++) if(++counter>=count) {++frames;counter=0;}
            assert(frames==unsigned(limit*2));
        }
    }
    for(const char* value:{"30","60","0"}) {int parsed=-1;assert(macross_fps_limit_parse(value,&parsed));}
    for(const char* value:{"","1","120","-1","60bad","nan"}) {int parsed=30;assert(!macross_fps_limit_parse(value,&parsed)&&parsed==30);}
    gate_ok=false;assert(macross_runtime_set_fps_unlocked(MACROSS_FPS_60)==MACROSS_FPS_30);
    assert(macross_runtime_frame_vblanks()==2);
    gate_ok=true;measured=false;assert(macross_runtime_set_fps_unlocked(MACROSS_FPS_UNLIMITED)==MACROSS_FPS_30);
    measured=true;_putenv_s("MACROSS_DT_STEP","0");
    assert(macross_runtime_set_fps_unlocked(MACROSS_FPS_60)==MACROSS_FPS_30);
    _putenv_s("MACROSS_DT_STEP","1");assert(macross_runtime_set_fps_unlocked(999)==MACROSS_FPS_30);
    puts("PASS 30/60/unlimited transitions, actual guest gates, migration parser and compatibility fallbacks");
}
