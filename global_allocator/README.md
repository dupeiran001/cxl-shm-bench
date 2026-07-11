# global_allocator — Artifact A2

Userspace library that turns a single shared CXL memory region (exposed
by the `interleave_dax` kernel module at `/dev/interleave_dax`) into an
ID-based shared-memory allocator with cross-host synchronization
primitives. It is the dependency underneath the graph workloads of
Artifact A5.

Within the shared region the library maintains a small metadata header
and a buddy-allocated object pool. Objects are identified by
user-chosen 64-bit IDs; the creator calls `shm_malloc_id(id, size)` and
every other host calls `shm_attach_id(id, size)` to map the same bytes
at the same offset.

## Layout

```
shm.{c,h}                core allocator (buddy + metadata)
barrier.{c,h}            multi-host barrier built on shm_mutex
shm_mutex.h              pluggable lock dispatcher
locks/{lfm,peterson,bakery}_lock.{c,h}   lock backends
cxl_atomic.h             software atomics (shm_mutex-backed)
common.h                 cacheline / fence helpers
tests/                   Criterion unit tests (local, tmpfs-backed)
verify/                  cross-host verifier (see verify/README.md)
Makefile                 library, unit-test, verifier targets
```

## Build

```sh
make                # builds lib + unit-test binary + verifier
make lib            # library only  -> build/libglobal_allocator.a
make test           # Criterion unit tests (requires libcriterion)
make verify         # cross-host verifier -> verify/verify_shm
make LOCK_IMPL=peterson    # switch lock backend: lfm | peterson | bakery
```

## Public API (`shm.h`)

```c
struct shm_arg {
  uint32_t host_id;      /* 0..host_num-1, unique per host */
  uint32_t host_num;     /* total participants */
  uint32_t flags;        /* SHM_ENABLE_RESET (host 0 only) */
  char     dax_dev[DEV_NAME_LENGTH];
  size_t   map_size;     /* bytes to mmap from dax_dev */
};

int32_t shm_enable(struct shm_arg);

void   *shm_malloc_id(uint64_t id, size_t size);   /* creator */
void   *shm_attach_id(uint64_t id, size_t size);   /* attacher */
int32_t shm_detach_id(uint64_t id);
int32_t shm_free_id(uint64_t id);
```

`shm_enable` on host 0 should pass `SHM_ENABLE_RESET` once per run to
initialize metadata; other hosts must call without the flag and will
retry until host 0 publishes a valid header.

## Synchronization helpers

Built into the library (all safe under CXL's single-host cache coherence
with software-managed cross-host visibility):

| Header | Primitive | Notes |
|---|---|---|
| `shm_mutex.h` + `locks/*` | `shm_mutex_t` | Dispatcher; compile-time `LOCK_IMPL={lfm,peterson,bakery}` picks backend |
| `barrier.h` | `shm_barrier_t`, `barrier_init`, `barrier_wait` | Multi-host barrier over `shm_mutex` + generation counter |
| `cxl_atomic.h` | `cxl_atomic64_{init,load,store,swap,cmpxchg,fetch_add}` | Software atomics wrapping a per-object `shm_mutex_t` (compile-time `LOCK_IMPL` backend) |

Native x86 atomics (`lock xchg`, `cmpxchg`, etc.) are **not** safe on
multi-host CXL memory because their correctness depends on hardware
cache coherence, which does not extend across hosts in our platform.
Use `cxl_atomic64_*` for cross-host atomicity.

## Testing

Two levels:

**1. Local unit tests (`tests/` + `make test`)**
Criterion-based suite that runs against a tmpfs file (e.g.
`/dev/shm/shm_enable_test_xxx`) so no CXL hardware is required. Covers
the allocator semantics, barrier correctness, lock semantics, and
`cxl_atomic64` concurrency.

```sh
make test && ./test --verbose
```

**2. Cross-host verifier (`verify/` + `make verify`)**
Two-host end-to-end check that exercises `shm_enable` →
`shm_malloc_id` → `shm_attach_id` → slice writes → `barrier_wait` →
read-and-verify. Requires `/dev/interleave_dax` on both hosts mapped to
the same physical region. See `verify/README.md` for invocation.

## Mock mode

Pass a tmpfs-backed file path in `arg.dax_dev` (e.g.
`/dev/shm/shm_mock`) instead of `/dev/interleave_dax`. The allocator
layout does not depend on the kernel module — only on a single
mappable file — so the full API works against tmpfs for local testing.
This is what `tests/` uses.
