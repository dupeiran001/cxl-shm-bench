// locks/peterson_lock.h - Peterson's Lock with Tournament Tree for N hosts
#ifndef PETERSON_LOCK_H
#define PETERSON_LOCK_H

#include "../common.h"
#include <stdalign.h>
#include <stdint.h>

#define SHM_MUTEX_MAGIC 0x50455445524E4C4BULL /* "PETERNLK" */

/*
 * Peterson's lock for 2 processes.
 * For N processes, we use a tournament tree of 2-process locks.
 *
 * Tree structure for N hosts (e.g., N=4):
 *        [lock 0]         <- final winner
 *       /        \
 *   [lock 1]   [lock 2]
 *    /   \       /   \
 *   H0   H1    H2   H3
 *
 * Number of locks needed: N-1 (for a complete binary tree)
 * Tree depth: ceil(log2(N))
 */

// Single 2-process Peterson lock
typedef struct {
  cacheline_u64 flag[2]; // Intention to enter (indexed 0 or 1)
  cacheline_u64 turn;    // Whose turn to wait (0 or 1)
} peterson_2p_t;

// Tournament tree of Peterson locks for N hosts
typedef struct {
  cacheline_u64 magic;
  cacheline_u64 num_hosts;
  peterson_2p_t locks[MAX_HOST_NUM]; // At most N-1 locks needed
} shm_mutex_t;

void shm_mutex_init(shm_mutex_t *m);
uint64_t shm_mutex_lock(shm_mutex_t *m, int id, int num_hosts);
void shm_mutex_unlock(shm_mutex_t *m, int id);

#define SHM_MUTEX_VALID(mptr)                                                  \
  (CACHELINE_LOAD(&(mptr)->magic) == SHM_MUTEX_MAGIC)

#endif // PETERSON_LOCK_H
