#ifndef _SHM_H
#define _SHM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEV_NAME_LENGTH 64

/* shm_enable flags */
#define SHM_ENABLE_RESET (1u << 0) /* only host_id==0 should use this */

struct shm_arg {
  uint32_t host_id;
  uint32_t host_num;
  uint32_t flags; /* SHM_ENABLE_* */
  char dax_dev[DEV_NAME_LENGTH];
  size_t map_size;
};

int32_t shm_enable(struct shm_arg);

void *shm_malloc_id(uint64_t id, size_t size);

void *shm_attach_id(uint64_t id, size_t size);

int32_t shm_detach_id(uint64_t id);

int32_t shm_free_id(uint64_t id);

#endif // _SHM_H
