# Generic Lock API Interface

All lock implementations must conform to this interface.

## Required Definitions

Each lock implementation must define the following types and functions:

```c
// Opaque mutex structure (implementation specific)
typedef struct { ... } shm_mutex_t;

// Initialize mutex. Must be called before use. Safe for multiple callers.
void shm_mutex_init(shm_mutex_t *m);

// Acquire lock. id = caller's host ID [0, num_hosts).
// Returns contention count or 0 on success, UINT64_MAX on error.
uint64_t shm_mutex_lock(shm_mutex_t *m, int id, int num_hosts);

// Release lock. Must be called by the lock holder.
void shm_mutex_unlock(shm_mutex_t *m, int id);

// Macro to check if mutex is properly initialized.
#define SHM_MUTEX_VALID(m) (...)

// Magic value used for validation
#define SHM_MUTEX_MAGIC ...
```

Include the appropriate implementation header based on `LOCK_IMPL_*` define in `shm_mutex.h`.
