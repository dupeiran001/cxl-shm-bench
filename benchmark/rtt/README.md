# rtt — publish/observe RTT microbenchmark

Measures cross-host round-trip, publish, and observe latencies over a
shared DAX mapping. Feeds Table II of the paper.

## Build

```sh
make
```

Static binary: `./rtt`. This tool is self-contained (no Abseil / numa
dependency, unlike the other benchmark tools).

## Usage summary

```
./rtt --mode=<server|client> [--iter=N] [--clwb] [--offset=N] [--dax_dev=<path>]
```

Default DAX device is `/dev/interleave_dax` (from Artifact A1). Override
with `--dax_dev=<path>` or the `DAX_DEV` env var when using the
`server.sh` / `client.sh` wrappers.

## Reproduction

For the exact server+client invocations that produce the Table II
values, see
[`../scripts/rtt/README.md`](../scripts/rtt/README.md).
That folder also contains the raw measurement dump (`raw/rtt.md` —
stdout blocks, one per flush primitive; the per-measurement values
are also tabulated in `scripts/README.md`) and the `server.sh` /
`client.sh` wrappers.
