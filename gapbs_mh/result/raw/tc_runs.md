
## mh g20 k50 OMP 8+8 

```bash
root@pxe:~/cxl_profiler# OMP_NUM_THREADS=8 ./gapbs_mh/tc_mh -g 20 -k 50 --host_id=0 --hosts=2     --dev=/dev/dax0.0 --shm-mb=16384 -n 10 -l
Mapped /dev/dax0.0 at 0x7fef26000000 (len=17179869184)
Using 172 threads for graph generation...
[Host 0] Building graph...
Generate Time:       0.10050
Build Time:          0.22856
Relabel:             0.13191
TC partition host 0 nodes [0, 13255) deg_sum=46067780 (50.0004%)
TC partition host 1 nodes [13255, 1048576) deg_sum=46067066 (49.9996%)
Restored 8 threads for TC execution.
[Host 0] Initialization complete.
[Host 0] Ready, waiting at initial barrier...
[Host 0] All hosts synchronized, starting TC
Graph has 1048576 nodes and 46067423 undirected edges for degree: 43
Trial Time:          3.45277
Trial Time:          3.43722
Trial Time:          3.43213
Trial Time:          3.43326
Trial Time:          3.43202
Trial Time:          3.44104
Trial Time:          3.43516
Trial Time:          3.43980
Trial Time:          3.43489
Trial Time:          3.43154
Average Time:        3.43698
```


## mp g20 k50 OMP 8
```bash
root@pxe:~/cxl_profiler#  OMP_NUM_THREADS=8 ./gapbs_mp/tc_mp -g 20 -k 50 --hosts=1 --shm-mb=16384 -n 10 -l --dev=/dev/dax0.0
ioctl CXLMOCK_IOC_REGISTER_HOSTMapped /dev/dax0.0 at 0x7efac6600000 (len=17179869184)
Using 172 threads for graph generation...
Generate Time:       0.10282
Build Time:          0.22383
Relabel:             0.13264
Restored 8 threads for TC execution.
Graph has 1048576 nodes and 46067423 undirected edges for degree: 43
Trial Time:          9.15487
Trial Time:          9.10048
Trial Time:          9.09943
Trial Time:          9.09981
Trial Time:          9.09968
Trial Time:          9.09944
Trial Time:          9.09675
Trial Time:          9.09893
Trial Time:          9.10152
Trial Time:          9.09788
Average Time:        9.10488
```

## mp g20 k50 OMP 16
```bash
root@pxe:~/cxl_profiler#  OMP_NUM_THREADS=16 ./gapbs_mp/tc_mp -g 20 -k 50 --hosts=1 --shm-mb=16384 -n 10 -l --dev=/dev/dax0.0             ioctl CXLMOCK_IOC_REGISTER_HOSTMapped /dev/dax0.0 at 0x7f2e48200000 (len=17179869184)
Using 172 threads for graph generation...
Generate Time:       0.11132
Build Time:          0.22680
Relabel:             0.13198
Restored 16 threads for TC execution.
Graph has 1048576 nodes and 46067423 undirected edges for degree: 43
Trial Time:          4.62306
Trial Time:          4.59001
Trial Time:          4.58937
Trial Time:          4.58911
Trial Time:          4.58905
Trial Time:          4.58758
Trial Time:          4.58953
Trial Time:          4.58866
Trial Time:          4.58971
Trial Time:          4.58892
Average Time:        4.59250
```


## unmodified g20 k50 OMP 8

```bash
root@pxe:~/cxl_profiler/gapbs# OMP_NUM_THREADS=8 ./tc -g 20 -k 50 -n 10 -l
Generate Time:       0.66670
Build Time:          1.69663
Graph has 1048576 nodes and 46067423 undirected edges for degree: 43
Relabel:             0.48067
Trial Time:          9.53067
Relabel:             0.48867
Trial Time:          9.52339
Relabel:             0.48932
Trial Time:          9.51756
Relabel:             0.49016
Trial Time:          9.52765
Relabel:             0.49041
Trial Time:          9.51719
Relabel:             0.48990
Trial Time:          9.52017
Relabel:             0.49008
Trial Time:          9.52244
Relabel:             0.48953
Trial Time:          9.51389
Relabel:             0.48986
Trial Time:          9.52702
Relabel:             0.48788
Trial Time:          9.51987
Average Time:        9.52199
```
