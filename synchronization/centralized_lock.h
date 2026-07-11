#ifndef BENCH_CENTRALIZED_LOCK_H
#define BENCH_CENTRALIZED_LOCK_H

#include "../global_allocator/common.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define CENTRAL_LOCK_MAGIC 0x43454e54524c4f43ULL /* "CENTRLOC" */

typedef struct {
  cacheline_u64 req;
  cacheline_u64 ack;
  cacheline_u64 rel;
  cacheline_u64 rel_ack;
} central_slot_t;

typedef struct {
  cacheline_u64 magic;
  cacheline_u64 ready;
  cacheline_u64 num_hosts;
  cacheline_u64 num_locks;
  cacheline_u64 channels;
} central_header_t;

typedef struct {
  central_header_t *hdr;
  central_slot_t *slots;
  int num_hosts;
  int num_locks;
  int channels;
} central_layout_t;

typedef struct {
  int lock_id;
  int channel;
  uint64_t seq;
} central_handle_t;

typedef struct {
  atomic_uint *slot_in_use;
  atomic_uint *next_slot;
  uint64_t *thread_seq;
  int num_locks;
  int channels;
  int threads;
} central_local_t;

size_t central_layout_bytes(int num_hosts, int num_locks, int channels);
void central_init_layout(void *slots_base, int num_hosts, int num_locks,
                         int channels);
central_layout_t central_attach_layout(void *slots_base, int num_hosts,
                                      int num_locks, int channels);

central_slot_t *central_slot_at(central_layout_t *layout, int lock_id,
                                int host_id, int channel);

void central_manager_run(central_layout_t *layout);
void central_manager_run_for(central_layout_t *layout, uint64_t duration_ns);
void central_set_debug(int enabled);

int central_local_init(central_local_t *local, int num_locks, int channels,
                       int threads);
void central_local_destroy(central_local_t *local);

int central_acquire(central_layout_t *layout, central_local_t *local, int host_id,
                    int thread_id, int lock_id, central_handle_t *handle,
                    uint64_t *poll_loops, uint64_t *retries);
int central_release(central_layout_t *layout, central_local_t *local, int host_id,
                    central_handle_t *handle, uint64_t *poll_loops);

#endif
