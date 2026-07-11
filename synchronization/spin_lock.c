#include "spin_lock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void spin_lock_init(spin_lock_t *lock) {
  int rc = pthread_spin_init(&lock->lock, PTHREAD_PROCESS_PRIVATE);
  if (rc != 0) {
    fprintf(stderr, "pthread_spin_init failed: %s\n", strerror(rc));
    abort();
  }
}

void spin_lock_lock(spin_lock_t *lock) {
  int rc = pthread_spin_lock(&lock->lock);
  if (rc != 0) {
    fprintf(stderr, "pthread_spin_lock failed: %s\n", strerror(rc));
    abort();
  }
}

void spin_lock_unlock(spin_lock_t *lock) {
  int rc = pthread_spin_unlock(&lock->lock);
  if (rc != 0) {
    fprintf(stderr, "pthread_spin_unlock failed: %s\n", strerror(rc));
    abort();
  }
}
