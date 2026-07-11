# global_allocator cross-host verifier

`verify_shm` exercises the full memory allocation API across two hosts that share the
same CXL memory via `/dev/interleave_dax`:

- Host 0 (`host1.sh`) creates a shared barrier and data object via
  `shm_malloc_id`; host 1 (`host2.sh`) attaches via `shm_attach_id`.
- Each round every host writes its own slice, barriers, reads all slices
  and verifies the expected pattern, then barriers again.
- Exits non-zero on the first verification mismatch.

## Prerequisites

On **both** hosts:

1. The `interleave_dax` kernel module is loaded and `/dev/interleave_dax`
   exists (see `../../interleave_dax/README.md`).
2. Both hosts have the same physical CXL region mapped through that
   device.
3. The binary is built:
   ```sh
   cd ..      # into global_allocator/
   make verify
   ```

## Run

Launch `host1.sh` on the first host and `host2.sh` on the second
(order is not critical — host 1 retries `shm_attach_id` until host 0 has
published the shared objects).

```sh
# host A
./host1.sh

# host B
./host2.sh
```

Both scripts accept the same env overrides:

| Variable  | Default               | Meaning                    |
|-----------|-----------------------|----------------------------|
| `ROUNDS`  | `100`                 | verification rounds        |
| `DAX_DEV` | `/dev/interleave_dax` | shared CXL device path     |

Example:

```sh
ROUNDS=1000 DAX_DEV=/dev/dax0.0 ./host1.sh
```

## Expected output

On success, both hosts print a progress line every 10 rounds and finish
with:

```
[host 0] all 100 rounds passed.
[host 1] all 100 rounds passed.
```

A slice-pattern mismatch prints the failing `(host, cacheline, round,
tag, got, expected)` and exits with code 2.
