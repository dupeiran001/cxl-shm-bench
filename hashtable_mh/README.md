# hashtable_mh — concurrent hash table on multi-host shared CXL memory

Artifact A7. a concurrent chained hash table living entirely in the
non-coherent CXL shared-memory window, exercised simultaneously by up to
four hosts. It extends the paper's read-dominated-workloads-fit thesis from
array-based graph kernels to a **pointer-chasing structure with fine-grained
synchronization**: lookups walk chains through explicitly invalidated
cachelines, and every bucket is guarded by a cross-host software lock.

## Design

- **Structure** (`src/shm_ht.{h,c}`): fixed power-of-two bucket array +
  chained nodes, all placed in shared memory through the artifact's global
  allocator (`shm_malloc_id` / `shm_attach_id`, artifact A2). Keys are
  `uint64`, values a fixed 64-byte payload (one cacheline).
- **Per-bucket locking**: each bucket embeds its own LFM lock
  (`shm_mutex_t`, Lamport's fast mutex — every field cacheline-padded); the
  chain-head pointer sits in its own cacheline. The LFM lock arbitrates one
  participant per host, so threads of the same host are serialized per
  bucket by a striped array of process-local pthread mutexes.
- **Per-host slabs**: chain nodes are bump-allocated from a per-host slab
  region inside the shared segment — no cross-host free-list contention;
  node references are offsets, valid on every host regardless of mapping
  address.
- **Visibility protocol** (memory is non-coherent between hosts):
  - LOOKUP: acquire bucket lock → `clflushopt`+`mfence` the bucket head and
    each chain node as traversed → read → release.
  - INSERT: write the node, `clwb`+`sfence`, **then** publish the new chain
    head, `clwb`+`sfence` (node is fully visible before it is reachable).
  - UPDATE: overwrite the 64-byte value line in place, `clwb`+`sfence`.
- The single-host `dram` configuration runs the **same code path** (locks +
  flushes); the only variable across configurations is the memory substrate.
- **Robustness**: every polling loop is bounded (attach retries, root-magic
  waits, barrier waits) — a dead peer produces a clean nonzero exit, not a
  hang. (Narrow exception: a peer dying *inside* a microsecond-scale lock
  critical section can still wedge the LFM lock; the reproduce script guards
  every invocation with `timeout` + `pkill`.)
- **Self-check**: after the timed phase every host (a) verifies a sample of
  preloaded keys is still readable with an intact key-echo, (b) round-trips
  its per-thread op counters through shared memory and compares with local
  counts, (c) inserts host-tagged check keys and verifies **every other
  host's** check keys after a barrier (cross-host visibility). Any failure
  increments the `error_count:` line and makes the run invalid (nonzero
  exit).

## Build

```sh
make            # builds ../global_allocator lib if missing, then ht_bench
```

Static `gcc -O3 -march=native` binary; `STATIC=0 make` if static glibc is
unavailable.

## Local validation (no hardware)

```sh
test/mock_test.sh
```

Builds, then runs a single-process smoke test plus a 2-"host" functional
test as two processes sharing a tmpfs file (`/dev/shm/...`) as the fake DAX
device. Asserts clean exits and `error_count: 0` everywhere.

## One-command reproduction

```sh
RUNS=3 hashtable_mh/reproduce.sh all       # or: dram|remote|shared2|shared4
QUICK=1 hashtable_mh/reproduce.sh all      # fast pipeline smoke test
```

Requires the repo deployed + built on g1..g4 at `/tmp/cxl_shm_profiling`
(see `scripts/prepare.sh`) and the shared window mapped. Configurations:

| config          | hosts | device                              |
|-----------------|-------|-------------------------------------|
| `dram`          | 1     | tmpfs file (`/dev/shm/ht_dram.bin`) |
| `remote_cxl`    | 1     | `/dev/dax0.0`                       |
| `shared_2hosts` | 2     | `/dev/interleave_dax` (4-module)    |
| `shared_4hosts` | 4     | `/dev/interleave_dax` (4-module)    |

Sweep: mixes 100/95/50 % reads × 8/16/32 threads per host × `RUNS` outer
repetitions; 2^20 buckets, 1M preloaded keys, 10 s per point. Rows append to
`result/raw/hashtable.csv`, then `scripts/plot_ht.py` regenerates the
figure.

## Manual invocation

```sh
# host 0 (creates + preloads, then waits at the shared-memory barrier)
./ht_bench --dev=/dev/interleave_dax --hosts=2 --host_id=0 --init \
    --threads=16 --mix=95 --buckets=$((1<<20)) --keys=1000000 \
    --slab-mb=256 --shm-mb=4096 --seconds=10

# host 1 (attaches; start within the attach timeout)
./ht_bench --dev=/dev/interleave_dax --hosts=2 --host_id=1 \
    --threads=16 --mix=95 --buckets=$((1<<20)) --keys=1000000 \
    --slab-mb=256 --shm-mb=4096 --seconds=10
```

Optional: `--zipf` (zipfian keys, θ=0.99; default uniform), `--sample=N`
(latency sampling period, default every 32nd op), `--attach-timeout` /
`--barrier-timeout` (seconds).

Every host prints a `HOST <i> RESULTS` block (ops, mops, p50/p95/p99 ns of
its sampled per-op latency, `error_count:`). Host 0 additionally prints one
machine-parsable line — latency percentiles are host 0's sample
distribution:

```
AGGREGATE mops=<sum of host rates> p50_ns=.. p95_ns=.. p99_ns=.. total_ops=.. hosts=.. errors_all=..
```

## Layout

```
hashtable_mh/
├── scripts/{plot_ht.py,README.md}
└── result/
    ├── raw/hashtable.csv     benchmark,hosts,threads_per_host,mix,run,mops,p50_ns,p95_ns,p99_ns
    └── plots/fig_ht_mops_t<T>.{pdf,png}
```

`benchmark` holds the configuration name (`dram`, `remote_cxl`,
`shared_2hosts`, `shared_4hosts`); `mops` is the cross-host aggregate
throughput of that run.
