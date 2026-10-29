#include "spu_workload.h"
#include "spu_recomp.h"
extern void spu_begin_image(int);
extern void macross_graphics_spu_recomp_register(void);
extern void macross_graphics_spu_func_00000000(spu_context*);
void macross_notify_register(void)
{
    spu_begin_image(10001);
    macross_notify_spu_recomp_register();
    spu_workload_register_img(0x4C091E2C0BD5C570ULL,
        macross_notify_spu_func_00000000,10001,"Macross raw SPURS job A62F00");
    spu_begin_image(10002);
    macross_graphics_spu_recomp_register();
    spu_workload_register_img(0x0AE6346F73BC04A7ULL,
        macross_graphics_spu_func_00000000,10002,"Macross captured raw SPURS job 0AE6346F73BC04A7");
    spu_begin_image(0);
}
