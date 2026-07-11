# Lock benchmark (CXL / non-coherent)

This benchmark evaluates several lock implementations on a shared DAX mapping,
including a centralized manager lock. It supports multi-host runs with multiple
threads per host, using a tiered local lock (pthread mutex) before the global
lock to serialize local threads.

## Build

```bash
make
```

## Common options

- `--lock=lfm|peterson|bakery|centralized|spin`
- `--hosts=N` total hosts
- `--host_id=ID` this host's id
- `--threads=N` threads per host
- `--locks=N` number of lock objects
- `--iters=N` iterations per lock per host (default 1000)
- `--seconds=N` run for N seconds per host (overrides `--iters`)
- `--hold_ns=N` time to hold lock (ns)
- `--gap_ns=N` gap after unlock (ns)
- `--channels=N` channels per host/lock (centralized only)
- `--dax_dev=PATH` DAX device (default /dev/dax0.0)
- `--offset=N` mmap offset (alignment depends on dax dev)
- `--daemon` run centralized manager (centralized only)
- `--init` initialize non-centralized lock state
- `--no_flush_stats` avoid flushing stats (spin lock only)

Notes:
- For non-centralized locks, pass `--init` on exactly one host to initialize
  the shared state. All other hosts wait for it.
- For centralized locks, `--daemon` initializes the shared state and **forks**
  a manager process; the parent continues as a normal host.
- `--lock=spin` uses a local `pthread_spinlock_t` and only supports
  `--hosts=1` (single process / host). It does not flush lines in the
  critical section.
- `--channels` controls how many independent message channels each
  (host, lock) pair has. Each channel consumes **4 cache lines**:
  request, grant, release, release-ack.

## Centralized lock

Start the daemon first on one host:

```bash
./lock_bench --lock=centralized --daemon --hosts=2 --host_id=0 --threads=1 --locks=8 --channels=2
```

Then start client instances:

```bash
./lock_bench --lock=centralized --hosts=2 --host_id=0 --threads=4 --locks=8 --channels=2
./lock_bench --lock=centralized --hosts=2 --host_id=1 --threads=4 --locks=8 --channels=2
```

## Non-centralized locks

```bash
./lock_bench --lock=lfm --hosts=2 --host_id=0 --threads=4 --locks=8 --init
./lock_bench --lock=lfm --hosts=2 --host_id=1 --threads=4 --locks=8
```

## Output

The benchmark prints:
- avg / p50 / p95 / p99 lock and unlock latency (ns)
- total retries
- polling loop counts (centralized)
- total run duration
- mutual-exclusion error count (reported by daemon for centralized lock)

## Centralized lock protocol

Unlike the decentralized locks below (LFM, Peterson, Bakery), where every host
runs the mutual-exclusion algorithm itself, the centralized lock routes every
acquire and release through a single **daemon** (one host started with
`--daemon`) that is the sole arbiter. Clients and the daemon exchange no network
messages — they communicate only through counters in shared CXL memory. Because
that memory is **not cache-coherent across hosts**, the writer flushes every
update and the reader re-fetches it (`cacheline_u64` fields via the
`CACHELINE_STORE`/`CACHELINE_LOAD` helpers); a plain shared flag would never
become visible on its own.

### Shared layout

The daemon initializes one **slot** per `(lock, host, channel)` triple
(`central_slot_at()` in `centralized_lock.h`). Each slot holds four
monotonically-increasing 64-bit sequence counters, each on its own cache line so
they can be flushed and observed independently:

| field     | written by | meaning                                       |
|-----------|------------|-----------------------------------------------|
| `req`     | client     | requesting the lock as request #`seq`         |
| `ack`     | daemon     | granted — you now hold request #`seq`         |
| `rel`     | client     | done — releasing request #`seq`               |
| `rel_ack` | daemon     | release of #`seq` acknowledged; slot is free  |

A **channel** is one request slot for a client; giving a host several channels
(`--channels=N`) lets several of its threads have requests outstanding on the
same lock at once. Using *sequence numbers* rather than booleans is what makes
this safe on non-coherent memory: a client polling a stale-but-flushed cache
line can never mistake a previous round's grant or release for the current one,
because it advances only when a counter equals *its own* `seq`.

### Client (requester) side — `central_acquire` / `central_release`

1. **Reserve a channel.** Atomically claim one of the host's free channels for
   this lock (local `slot_in_use`/`next_slot` state — no shared-memory traffic
   yet).
2. **Request.** Take a fresh `seq`, write `req = seq`, and flush it so the
   daemon can see the request.
3. **Await the grant.** Poll `ack` until it equals `seq`; at that point the
   daemon has granted the lock to this channel and the thread holds it.
4. **Release.** After the critical section, write `rel = seq` (and flush) to
   signal the daemon.
5. **Await the confirm.** Poll `rel_ack` until it equals `seq`, then return the
   channel to the local free set. Waiting for `rel_ack` (rather than leaving
   right after step 4) guarantees the daemon has fully retired this request
   before the channel is reused.

### Daemon (arbiter) side — `central_manager_run`

Looping over the locks, for each lock the daemon:

1. **Selects one pending request** — a slot whose `req` has advanced past its
   `ack` — scanning round-robin across all hosts and channels so no requester
   starves.
2. **Grants it** — writes `ack = seq`, then **waits for that client's
   `rel = seq`**. It grants no other request for this lock until the current
   holder releases; this one-holder-at-a-time loop is precisely what enforces
   mutual exclusion.
3. **Retires it** — writes `rel_ack = seq`, then moves on to the next request.

Every acquire and release for a lock therefore funnels through this single
daemon over CXL, so the daemon host becomes the scaling bottleneck as hosts,
locks, and threads grow — the cost the paper's centralized-lock curves quantify
against the decentralized locks.

## Mutual exclusion check

On each lock acquisition, the thread:
- checks a shared `in_cs` marker (error if already set),
- increments a shared `global_counter`,
- writes a per-host counter (one cache line per host).

The centralized daemon prints the error counter if it changes.

## Reproducing Figure 5 and Figure 6

The multi-host sweep that produces the paper's figures is driven by
the runners in `scripts/`; see
[`scripts/README.md`](scripts/README.md) for the full
runbook (prerequisites, `run.sh` invocation, and how to regenerate the
plots).
