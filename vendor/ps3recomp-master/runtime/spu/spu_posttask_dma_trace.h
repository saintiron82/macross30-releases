#ifndef SPU_POSTTASK_DMA_TRACE_H
#define SPU_POSTTASK_DMA_TRACE_H
#include "spu_context.h"
/* Optional diagnostics only. Always defined by spu_channels.c for SDK linking. */
void spu_posttask_dma_trace_mark(spu_context* ctx, unsigned family);
#endif
