/* Format rows learned from culling holes do not prove vertex-output support.
 * geom6's 0x29B0 callback accepts output ids 0x41..0x47; 0x40 returns without
 * packing vertices. Recorded pos-only scheme-0 jobs therefore transmit a
 * different in-place IO span than the native raw-input copy. Keep this
 * confirmed shape on SPU before candidate allocation or trust lookup. */
#ifndef EDGE_SKIN_OUTPUT_MODE_H
#define EDGE_SKIN_OUTPUT_MODE_H
#include <stdint.h>
static inline int es_spu_only_output_mode(int image,uint32_t input,uint32_t output,
                                         uint32_t scheme,int pos_only) {
    return image==10011 && input==0x44 && output==0x40 && scheme==0 && pos_only;
}
#endif
