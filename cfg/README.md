# Kernel + platform configuration

This directory ships the exact kernel and BIOS configuration used for
every measurement in the paper.

## `bios-attributes.json`

The full UEFI/BIOS setup of the testbed hosts (Supermicro
`SSG-222B-NE3X24R`, BIOS `1.4b`, 2025-11-27) — the reference
configuration for all four hosts, captured read-only from the BMC via
Redfish:

```sh
curl -sk -u "$BMC_USER:$BMC_PASS" \
  https://<bmc-address>/redfish/v1/Systems/1/Bios   # .Attributes
```

Notes:

- Captured on host `g1`; ~1,340 attributes, key-sorted. Boot-entry
  attributes (`UEFIBootOption_*`, `UEFINETWORKBootOption_*`) embed
  per-host NIC MACs and PXE ordering and necessarily differ between
  hosts; all password-type attributes are empty.
- Known per-host deviations from this reference at measurement time
  (documented for full transparency): `g2` had `XPTPrefetch`/
  `KTIPrefetch` Disabled and `DCUStreamerPrefetcher`/`HomelessPrefetch`
  on Auto; `g4` had `IIOLLCWaysMask=0xFFFF` and Resizable BAR enabled;
  `g1` had PCIe Gen5 `EqualizationBypassToHighestRate` enabled. Host
  roles are fixed by the reproduce scripts, so these settings are part
  of the recorded environment.

## `config-7.0.0-rc5-cxl`

The kernel `.config` of the paper's **production (debug-OFF)** kernel.

| | |
|---|---|
| **Source** | mainline `torvalds/linux` |
| **Commit** | `0138af2472dfdef0d56fc4697416eaa0ff2589bd` |
| **Describe** | `v7.0-rc5-79-g0138af2472df` (v7.0-rc5 + 79 commits ≈ 7.0-rc6) |
| **Reported version** | `7.0.0-rc5+` |
| **Toolchain** | GCC 15.2.1 + GNU ld/as (the tree is GCC-built — do **not** build with clang/lld) |

### Key features enabled

- `CONFIG_DEV_DAX`, `CONFIG_DEV_DAX_HMEM` — DAX devices for the CXL windows
- `CONFIG_CXL_BUS`, `CONFIG_CXL_MEM` — CXL Type-3 support
- `CONFIG_EFI_SOFT_RESERVE` — soft-reserve the switch-provided CXL window
- `CONFIG_INFINIBAND`, `CONFIG_INFINIBAND_USER_ACCESS`,
  `CONFIG_INFINIBAND_ADDR_TRANS`, `CONFIG_MLX5_INFINIBAND` — verbs for the
  RDMA-vs-CXL comparison (`benchmark/reproduce.sh rdma`, Table III)
- `CONFIG_NUMA`, `CONFIG_TRANSPARENT_HUGEPAGE`

## `Module.symvers-7.0.0-rc5-cxl`

The exported-symbol table (`Module.symvers`) produced by the full build of
the exact kernel above. `scripts/fetch_kernel.sh` installs it into the
fetched source tree so the A1 module (`interleave_dax.ko`) can be built —
with full `modpost` symbol checking — against a tree that is only
`modules_prepare`d, without rebuilding the whole kernel.
(`CONFIG_MODVERSIONS` is off in this config, so no per-symbol CRCs are
involved; the module loads on the PXE kernel with matching vermagic
`7.0.0-rc5+`.)

## Building the kernel

```sh
git clone https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
cd linux
git checkout 0138af2472dfdef0d56fc4697416eaa0ff2589bd
cp /path/to/cfg/config-7.0.0-rc5-cxl .config
make olddefconfig        # fills any new defaults; should be a no-op here
make -j"$(nproc)"        # GCC toolchain — do NOT pass CC=clang
```

The out-of-tree interleaving kernel module (artifact A1, `interleave_dax/`)
builds against this same tree. For the module alone, a full kernel build is
unnecessary — `scripts/fetch_kernel.sh` automates the shallow fetch,
config, `modules_prepare`, and `Module.symvers` installation.
