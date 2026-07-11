## mh g25 k50 OMP 8+8 
```bash
root@pxe:~/cxl_profiler#   OMP_NUM_THREADS=8 ./gapbs_mh/pr_mh -g 25 -k 50 --host_id=0 --hosts=2     --dev=/dev/dax0.0 --shm-mb=16384 -n 1 -v --sync-k 1 -l
Mapped /dev/dax0.0 at 0x7f29c1600000 (len=17179869184)
[Host 0] Building graph...
Generate Time:       1.95657
Build Time:          1.99945
[Host 0] Initializing PR arrays...
[Host 0] Initialization complete.
[Host 0] Ready, waiting at initial barrier...
[Host 0] All hosts synchronized, starting PR
Graph has 33554432 nodes and 1592793612 undirected edges for degree: 47
iter    0  local=   0.59613  total=   1.19538  time=   1.55079  sync=1
iter    1  local=   0.04536  total=   0.09106  time=   1.54652  sync=1
iter    2  local=   0.00323  total=   0.00649  time=   1.51840  sync=1
iter    3  local=   0.00046  total=   0.00087  time=   1.51954  sync=1
iter    4  local=   0.00008  total=   0.00017  time=   1.51530  sync=1
iter    5  local=   0.00005  total=   0.00009  time=   1.53920  sync=1
Trial Time:          9.19010
Total Error:         0.00005
Verification:           PASS
Average Time:        9.19010
```

## mp g25 k50 OMP 8

```bash
root@pxe:~/cxl_profiler# OMP_NUM_THREADS=8 ./gapbs_mp/pr_mp -g 25 -k 50 --hosts=1 --shm-mb=16384 -n 1 -v -l
ioctl CXLMOCK_IOC_REGISTER_HOSTMapped /dev/cxlmock at 0x7f7602362000 (len=17179869184)
Host 0: Making Local Graph...
Generate Time:       1.92717
Build Time:          2.00817
Host 0: Building Shared Graph (Parallel)...
Host 0: InitPrArrays (Parallel)...
Host 0: InitPrArrays Done. Setting Ready.
Host 0: PrintStats...
Graph has 33554432 nodes and 1592793612 undirected edges for degree: 47
Host 0: PrintStats Done.
iter    0  local=   1.18621  total=   1.18621  time=   2.28841  sync=1
iter    1  local=   0.06330  total=   0.06330  time=   2.26462  sync=1
iter    2  local=   0.00202  total=   0.00202  time=   2.26590  sync=1
iter    3  local=   0.00009  total=   0.00009  time=   2.26304  sync=1
Trial Time:          9.08214
Total Error:         0.00001
Verification:           PASS
Average Time:        9.08214
```

## unmodified g25 k50 OMP 8

```bash
root@pxe:~/cxl_profiler# OMP_NUM_THREADS=8 ./gapbs/pr -g 25 -k 50 -n 1 -v -l
Generate Time:       26.30349
Build Time:          53.49321
Graph has 33554432 nodes and 1592793612 undirected edges for degree: 47
    0                1.18621
    1                0.06330
    2                0.00202
    3                0.00009
Trial Time:          6.59207
Total Error:         0.00001
Verification:           PASS
Verification Time:   14.67244
Average Time:        6.59207
```
