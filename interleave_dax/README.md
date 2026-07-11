# interleave_dax

Out-of-tree kernel module that exposes `/dev/interleave_dax`, a PFN-mapped
character device that backs a single virtual mapping with software
page-granularity **weighted interleave** across one or more physically
contiguous CXL memory regions.

This addresses the limitation: the CXL switch
in our platform exposes each device (or a contiguous group) as a single DAX
region, and neither the switch nor the kernel's native interleave policy
can stripe pages within a single DAX device at page granularity. This module does it in
software: on page fault it maps a logical page index to a PFN using a
weighted round-robin across user-declared GiB ranges relative to a common
physical base.

## Build

```sh
make                            # uses /lib/modules/$(uname -r)/build
make KDIR=/path/to/kernel/build # build against a different kernel tree
```

The Makefile uses `CC=clang`. Tested with kernel 6.16 and clang 22.1.1 per
the paper's environment; it should build with any kernel whose headers are
installed.

## Load parameters

The module takes two required insmod parameters:

| Parameter   | Type           | Meaning |
|-------------|----------------|---------|
| `base_phys` | unsigned long  | Physical base address of the interleave window |
| `config`    | string         | `"<start>-<end>:weight[,<start>-<end>:weight...]"` — each range is in **GiB relative to `base_phys`**, `weight` is pages per round |

On page fault, logical pages in the VMA are served by weighted
round-robin across the configured ranges: in each round the module
hands out `weight_i` consecutive pages from range `i`, then advances
to range `i+1`, and so on. Access past the backing pages of any range
returns `SIGBUS`. Up to `IL_MAX_RANGES = 32` ranges are supported.

## Finding `base_phys` from SRAT

The ACPI SRAT table lists each memory affinity entry (including CXL
hot-pluggable regions) with its physical base and length. Dump it from
the kernel log:

```sh
sudo dmesg | grep -i SRAT
```

Each CXL memory device appears as one (or more) `SRAT: Node ... Memory`
lines of the form:

```
ACPI: SRAT: Node 2 PXM 2 [mem 0x4080000000-0x40ffffffff] hotplug
...
```

Use the starting address of the **first** range you want to include as
`base_phys`, then express every subsequent range as a `start-end` pair
in GiB **relative to that base**. For example, if SRAT shows eight
adjacent 128 GiB CXL regions starting at `0x4080000000`, the equal-weight
config is:

```
base_phys=0x4080000000
config=0-128:1,128-256:1,256-384:1,384-512:1,512-640:1,640-768:1,768-896:1,896-1024:1
```


## Load and verify

The included `install.sh` is a worked example for an 8×128 GiB layout
starting at `0x4080000000` with equal weights:

```sh
sudo ./install.sh
```

Or invoke `insmod` directly:

```sh
sudo insmod interleave_dax.ko \
    base_phys=0x4080000000 \
    config="0-128:1,128-256:1,256-384:1,384-512:1,\
512-640:1,640-768:1,768-896:1,896-1024:1"

ls -l /dev/interleave_dax
dmesg | tail
```

On success, dmesg reports the parsed range count and total weight, and
`/dev/interleave_dax` appears as a misc char device.

## Use from userspace

A single `mmap(/dev/interleave_dax, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)`
yields a VMA whose successive pages are striped across the configured
ranges by the weighted round-robin described above. The mapping is
`VM_PFNMAP | VM_IO`; standard load/store works, but the pages are not
in the page cache and `fork()` does not propagate the mapping.

## Validate

`validate.sh` confirms the module is loaded, the device node exists,
and drives a read bandwidth test against `/dev/interleave_dax` using
`fio` (`ioengine=mmap`, since the module only implements `.mmap`).

```sh
sudo ./validate.sh                        # defaults: rw=read bs=2m size=8G numjobs=8
sudo RW=read JOBS=16 SIZE=16G ./validate.sh
```

Interpretation: aggregate bandwidth should scale roughly with the number
of ranges in `config` (up to the PCIe/switch ceiling). Flat bandwidth as
`config` grows from 1 to N ranges indicates interleave is not working.
Keep `SIZE × JOBS` within the total backing of the configured ranges;
reads past the end return `SIGBUS`.

## Unload

```sh
sudo rmmod interleave_dax
```

