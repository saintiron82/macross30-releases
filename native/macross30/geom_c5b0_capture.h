#ifndef MACROSS_GEOM_C5B0_CAPTURE_H
#define MACROSS_GEOM_C5B0_CAPTURE_H
#include "../../vendor/ps3recomp-master/runtime/spu/spu_context.h"
#ifdef __cplusplus
extern "C" {
#endif
extern int g_macross_geom_c5b0_capture_on;
void macross_geom_c5b0_capture_install(void);
int macross_geom_c5b0_capture_pre(spu_context*);
void macross_geom_c5b0_capture_post(spu_context*,int);
#ifdef __cplusplus
}
#endif
#endif

