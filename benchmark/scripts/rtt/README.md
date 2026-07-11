# rtt Scripts

Reproduces **Table II** of the paper: cross-host software-coherence
publish/observe/RTT latency over a shared DAX mapping. Two hosts share
a CXL region exposing `/dev/interleave_dax` (or similar DAX device);
each runs one role (server or client) and prints summary latency at
the end.

## Binary

Built from `benchmark/rtt/rtt.c` via `benchmark/rtt/Makefile`:

```sh
make -C ../../rtt
```

## Common parameters

- `--dax_dev=<path>` — shared DAX device (default `/dev/interleave_dax`).
  Must point at the same physical region on both hosts.
- `--iter=<N>` — iterations per side (default 1,000,000). Both sides
  must agree.
- `--clwb` — use `clwb` instead of `clflushopt` for write flushes.
- `--offset=<N>` — byte offset into the DAX mapping (must be 4 KiB
  aligned). Use different offsets if you run concurrent pairs on the
  same device.

The `server.sh` / `client.sh` wrappers expose these as `DAX_DEV`,
`ITER`, `FLUSH`, `OFFSET` env vars.

## Reproduce Table II

One row of Table II per flush primitive. Start the server on host 1
first; then start the client on host 2. When both finish, each prints
the measured publish/observe/RTT/one-way latencies and the average
poll loop count.

### clflushopt (default)

```sh
# host 1
FLUSH=clflushopt ITER=1000000 ./server.sh

# host 2
FLUSH=clflushopt ITER=1000000 ./client.sh
```

### clwb

```sh
# host 1
FLUSH=clwb ITER=1000000 ./server.sh

# host 2
FLUSH=clwb ITER=1000000 ./client.sh
```

Each run prints something like:

```
--- Latency Test Results ---
Total iterations: 1000000
Total time:       ... seconds
Average RTT:      3050.84 nanoseconds
Average One-Way:  1525.42 nanoseconds
Publish latency:  1113.50 nanoseconds
Visibility lat.:  1904.01 nanoseconds
Avg poll loops:   3.06
---------------------------
```

Record the five numbers (Publish, Visibility, Avg poll loops, RTT,
One-Way) for each flush primitive. The expected magnitudes from our
paper run are archived in `../../result/rtt/raw/rtt.md` (one stdout block per
`method`), and the per-measurement values are also tabulated below.

## Expected magnitudes

From our 4-host Intel Xeon 6787P + Micron CZ120 CXL-DSM platform:

| measurement       | clflushopt | clwb    |
|-------------------|-----------:|--------:|
| Publish (ns)      |    1113.50 |  615.53 |
| Observe (ns)      |    1904.01 | 1298.80 |
| Avg poll loops    |       3.06 |    2.01 |
| RTT (ns)          |    3050.84 | 1948.21 |
| One-way (ns)      |    1525.42 |  974.10 |
