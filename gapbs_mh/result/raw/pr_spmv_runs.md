
## mh g25 k50 OMP 8+8

```bash
root@pxe:~/cxl_profiler#   OMP_NUM_THREADS=8 ./gapbs_mh/pr_spmv_mh -g 25 -k 50 --host_id=0 --hosts=2     --dev=/dev/dax0.0 --shm-mb=16384 -n 2 -l -v
Mapped /dev/dax0.0 at 0x7f1e28800000 (len=17179869184)
Using 172 threads for graph generation...
[Host 0] Building graph...
Generate Time:       1.91237
Build Time:          2.00393
Restored 8 threads for PR execution.
[Host 0] Initialization complete.
[Host 0] Ready, waiting at initial barrier...
[Host 0] All hosts synchronized, starting PR
Graph has 33554432 nodes and 1592793612 undirected edges for degree: 47
iter    0  local=   0.60007  total=   0.60007  time=   1.58086  sync=1
iter    1  local=   0.05400  total=   0.05400  time=   1.57192  sync=1
iter    2  local=   0.00559  total=   0.00559  time=   1.56893  sync=1
iter    3  local=   0.00096  total=   0.00096  time=   1.56572  sync=1
iter    4  local=   0.00018  total=   0.00018  time=   1.55968  sync=1
iter    5  local=   0.00009  total=   0.00009  time=   1.56592  sync=1
Trial Time:          9.41336
iter    0  local=   0.60023  total=   0.60023  time=   1.53891  sync=1
iter    1  local=   0.05852  total=   0.05852  time=   1.53601  sync=1
iter    2  local=   0.00618  total=   0.00618  time=   1.54061  sync=1
iter    3  local=   0.00106  total=   0.00106  time=   1.53339  sync=1
iter    4  local=   0.00017  total=   0.00017  time=   1.53994  sync=1
iter    5  local=   0.00004  total=   0.00004  time=   1.53821  sync=1
Trial Time:          9.22738
Total Error:         0.00002
Verification:           PASS
Average Time:        9.32037
```


## mp g25 k50 OMP 16
```bash
root@pxe:~/cxl_profiler#  OMP_NUM_THREADS=16 ./gapbs_mp/pr_spmv_mp -g 25 -k 50 --hosts=1 --shm-mb=16384 -n 2 -l --dev=/dev/dax0.0
ioctl CXLMOCK_IOC_REGISTER_HOSTMapped /dev/dax0.0 at 0x7f1bad000000 (len=17179869184)
Using 172 threads for graph generation...
Generate Time:       2.23895
Build Time:          1.99434
Restored 16 threads for PR execution.
Graph has 33554432 nodes and 1592793612 undirected edges for degree: 47
iter    0  local=   1.20556  total=   1.20556  time=   1.49215  sync=1
iter    1  local=   0.11755  total=   0.11755  time=   1.48564  sync=1
iter    2  local=   0.01242  total=   0.01242  time=   1.48167  sync=1
iter    3  local=   0.00214  total=   0.00214  time=   1.48259  sync=1
iter    4  local=   0.00033  total=   0.00033  time=   1.48213  sync=1
iter    5  local=   0.00008  total=   0.00008  time=   1.48186  sync=1
Trial Time:          8.90627
iter    0  local=   0.00002  total=   0.00002  time=   1.48206  sync=1
Trial Time:          1.48210
Average Time:        5.19419
```

## mp g25 k50 OMP 16
```bash
root@pxe:~/cxl_profiler#  OMP_NUM_THREADS=8 ./gapbs_mp/pr_spmv_mp -g 25 -k 50 --hosts=1 --shm-mb=16384 -n 2 -l --dev=/dev/dax0.0
ioctl CXLMOCK_IOC_REGISTER_HOSTMapped /dev/dax0.0 at 0x7f86e3000000 (len=17179869184)
Using 172 threads for graph generation...
Generate Time:       1.92219
Build Time:          1.99341
Restored 8 threads for PR execution.
Graph has 33554432 nodes and 1592793612 undirected edges for degree: 47
iter    0  local=   1.20556  total=   1.20556  time=   2.90832  sync=1
iter    1  local=   0.11755  total=   0.11755  time=   2.90469  sync=1
iter    2  local=   0.01242  total=   0.01242  time=   2.90896  sync=1
iter    3  local=   0.00214  total=   0.00214  time=   2.89446  sync=1
iter    4  local=   0.00033  total=   0.00033  time=   2.91091  sync=1
iter    5  local=   0.00008  total=   0.00008  time=   2.90331  sync=1
Trial Time:          17.43089
iter    0  local=   0.00002  total=   0.00002  time=   2.90679  sync=1
Trial Time:          2.90683
Average Time:        10.16886
```

## unmodified g25 k50 OMP 8
```bash
root@pxe:~/cxl_profiler#  OMP_NUM_THREADS=8 ./gapbs/pr_spmv -g 25 -k 50 -n 2 -l -v     Generate Time:       26.37797
Build Time:          54.00205
Graph has 33554432 nodes and 1592793612 undirected edges for degree: 47
    0                1.20556
    1                0.11755
    2                0.01242
    3                0.00214
    4                0.00033
    5                0.00008
Trial Time:          9.50127
Total Error:         0.00002
Verification:           PASS
Verification Time:   14.77399
    0                1.20556
    1                0.11755
    2                0.01242
    3                0.00214
    4                0.00033
    5                0.00008
Trial Time:          9.77705
Total Error:         0.00002
Verification:           PASS
Verification Time:   14.05080
Average Time:        9.63916
```
