#ifndef MACROSS_SPU_QUEUE_BACKOFF_H
#define MACROSS_SPU_QUEUE_BACKOFF_H
struct spu_context;
void macross_spu_queue_retry(struct spu_context* ctx);
void macross_spu_queue_activity(struct spu_context* ctx);
#endif
