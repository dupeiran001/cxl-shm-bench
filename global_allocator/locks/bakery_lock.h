#ifndef BAKERY_LOCK_H
#define BAKERY_LOCK_H

#include "../common.h"

#define SHM_MUTEX_MAGIC 0xBAAAAAADBAAAAAADULL

/*
 * Lamport's Bakery Algorithm for Mutual Exclusion
 *
 * Properties:
 * - Starvation-free (FCFS based on ticket number)
 * - Safe for N hosts
 * - Requires sequential consistency (enforced via fences)
 */

typedef struct {
  cacheline_u64 magic;
  cacheline_u64 number[MAX_HOST_NUM];
  cacheline_u64 entering[MAX_HOST_NUM];
} shm_mutex_t;

void shm_mutex_init(shm_mutex_t *m);
uint64_t shm_mutex_lock(shm_mutex_t *m, int id, int num_hosts);
void shm_mutex_unlock(shm_mutex_t *m, int id);

#define SHM_MUTEX_VALID(m) (CACHELINE_LOAD(&(m)->magic) == SHM_MUTEX_MAGIC)

/* Backward compatibility aliases */
typedef shm_mutex_t lfm_mutex;
#define lfm_mutex_init shm_mutex_init
#define lfm_mutex_lock shm_mutex_lock
#define lfm_mutex_unlock shm_mutex_unlock

#endif // BAKERY_LOCK_H
