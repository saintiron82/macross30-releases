/* Persistent full LS per chain; no raw pointer escapes a read lock.
 * Up to 32 retained nonzero handles, lazy allocation; retired slots are reusable.
 * No eviction/overwrite of another handle on capacity or allocation failure.
 * Caller retires only after its generation has no active jobs, under lifecycle lock.
 */
#ifndef SPURS_JOB_SNAPSHOT_H
#define SPURS_JOB_SNAPSHOT_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define SJS_CAPACITY 32u
#define SJS_LS_BYTES 0x40000u
#ifndef SJS_ALLOC
#define SJS_ALLOC malloc
#endif
#ifdef _WIN32
#include <windows.h>
static SRWLOCK sjs_lock=SRWLOCK_INIT;
#define SJS_WRITE_LOCK() AcquireSRWLockExclusive(&sjs_lock)
#define SJS_WRITE_UNLOCK() ReleaseSRWLockExclusive(&sjs_lock)
#define SJS_READ_LOCK() AcquireSRWLockShared(&sjs_lock)
#define SJS_READ_UNLOCK() ReleaseSRWLockShared(&sjs_lock)
#else
#include <stdatomic.h>
static atomic_flag sjs_lock=ATOMIC_FLAG_INIT;
static void sjs_take(void){while(atomic_flag_test_and_set_explicit(&sjs_lock,memory_order_acquire)) {}}
#define SJS_WRITE_LOCK() sjs_take()
#define SJS_WRITE_UNLOCK() atomic_flag_clear_explicit(&sjs_lock,memory_order_release)
#define SJS_READ_LOCK() SJS_WRITE_LOCK()
#define SJS_READ_UNLOCK() SJS_WRITE_UNLOCK()
#endif
typedef struct {uint32_t handle;uint8_t* bytes;} sjs_slot;
static sjs_slot sjs_slots[SJS_CAPACITY];
static int sjs_publish(uint32_t handle,const uint8_t* bytes){
 if(!handle||!bytes)return 0;
 SJS_WRITE_LOCK();unsigned index=0,empty=SJS_CAPACITY;
 for(;index<SJS_CAPACITY;index++){
  if(sjs_slots[index].handle==handle)break;
  if(!sjs_slots[index].handle&&empty==SJS_CAPACITY)empty=index;
 }
 if(index==SJS_CAPACITY){
  if(empty==SJS_CAPACITY){SJS_WRITE_UNLOCK();return 0;}
  uint8_t* storage=(uint8_t*)SJS_ALLOC(SJS_LS_BYTES);
  if(!storage){SJS_WRITE_UNLOCK();return 0;}
  index=empty;sjs_slots[index].bytes=storage;
  /* Key and full bytes become visible together only after unlock. */
  sjs_slots[index].handle=handle;
 }
 memcpy(sjs_slots[index].bytes,bytes,SJS_LS_BYTES);
 SJS_WRITE_UNLOCK();return 1;
}
static int sjs_retire(uint32_t handle){
 if(!handle)return 0;
 int found=0;SJS_WRITE_LOCK();
 for(unsigned i=0;i<SJS_CAPACITY;i++)if(sjs_slots[i].handle==handle){
  free(sjs_slots[i].bytes);sjs_slots[i].bytes=NULL;sjs_slots[i].handle=0;found=1;break;
 }
 SJS_WRITE_UNLOCK();return found;
}
static int sjs_read(uint32_t handle,uint32_t offset,uint8_t* out,uint32_t size){
 if(!handle||!out||!size||size>8||offset>SJS_LS_BYTES-size)return 0;
 int found=0;SJS_READ_LOCK();
 for(unsigned i=0;i<SJS_CAPACITY;i++)if(sjs_slots[i].handle==handle){memcpy(out,sjs_slots[i].bytes+offset,size);found=1;break;}
 SJS_READ_UNLOCK();return found;
}
#endif
