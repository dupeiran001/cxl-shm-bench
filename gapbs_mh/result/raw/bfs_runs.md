


## mh g25 k50 OMP 8+8

```bash
root@pxe:~/cxl_profiler#  OMP_NUM_THREADS=8 ./gapbs_mh/bfs_mh -g 25 -k 50 --host_id=0 --hosts=2 --dev=/dev/dax0.0 --shm-mb=16384 -n 10 -l
Mapped /dev/dax0.0 at 0x7f0013c00000 (len=17179869184)
Using 172 threads for graph generation...
Generate Time:       1.92078
Build Time:          2.00286
Restored 8 threads for BFS execution.
[Host 0] Initialization complete. Waiting for other hosts...
[Host 0] Ready, waiting at initial barrier...
[Host 0] All hosts synchronized, starting BFS
Graph has 33554432 nodes and 1592793612 undirected edges for degree: 47
td 0 frontier=2 time=0.005870 msg_send=0.000000 msg_recv=0.000001 local=1 remote=1 lr=0.500000 avg_lr=0.500000 avg_rr=0.500000
td 1 frontier=94458 time=0.005851 msg_send=0.000006 msg_recv=0.000829 local=47921 remote=47469 lr=0.502369 avg_lr=0.498691 avg_rr=0.501309
bu 2 awake=16999837 time=0.399560 u_scanned=33459971 v_checked=53871932 curr_true=16999837
bu 3 awake=4683540 time=0.142991 u_scanned=16460134 v_checked=4701680 curr_true=4683540
bu 4 awake=7158 time=0.012621 u_scanned=11776594 v_checked=10019 curr_true=7158
td 5 frontier=4 time=0.006618 msg_send=0.000006 msg_recv=0.000022 local=3523 remote=3647 lr=0.491353 avg_lr=0.491305 avg_rr=0.508695
td 6 frontier=0 time=0.005752 msg_send=0.000001 msg_recv=0.000017 local=3 remote=1 lr=0.750000 avg_lr=0.833333 avg_rr=0.166667
Trial Time:          0.77187
td 0 frontier=4 time=0.005733 msg_send=0.000000 msg_recv=0.000001 local=1 remote=3 lr=0.250000 avg_lr=0.250000 avg_rr=0.750000
td 1 frontier=44558 time=0.005815 msg_send=0.000006 msg_recv=0.000468 local=23203 remote=23433 lr=0.497534 avg_lr=0.496808 avg_rr=0.503192
bu 2 awake=15461794 time=0.397103 u_scanned=33509869 v_checked=70928884 curr_true=15461794
bu 3 awake=6266720 time=0.184818 u_scanned=18048075 v_checked=6298302 curr_true=6266720
bu 4 awake=11916 time=0.011006 u_scanned=11781355 v_checked=14783 curr_true=11916
td 5 frontier=7 time=0.006558 msg_send=0.000007 msg_recv=0.000028 local=5978 remote=5963 lr=0.500628 avg_lr=0.500575 avg_rr=0.499425
td 6 frontier=0 time=0.005696 msg_send=0.000000 msg_recv=0.000001 local=4 remote=3 lr=0.571429 avg_lr=0.700000 avg_rr=0.300000
Trial Time:          0.80127
td 0 frontier=30 time=0.005779 msg_send=0.000000 msg_recv=0.000021 local=15 remote=15 lr=0.500000 avg_lr=0.500000 avg_rr=0.500000
td 1 frontier=544387 time=0.007286 msg_send=0.000053 msg_recv=0.008113 local=357663 remote=356804 lr=0.500601 avg_lr=0.498719 avg_rr=0.501
bu 2 awake=19398418 time=0.412191 u_scanned=33010014 v_checked=31140436 curr_true=19398418
bu 3 awake=1840177 time=0.070380 u_scanned=13611596 v_checked=1846457 curr_true=1840177
bu 4 awake=1986 time=0.009896 u_scanned=11771419 v_checked=4842 curr_true=1986
td 5 frontier=1 time=0.006077 msg_send=0.000001 msg_recv=0.000016 local=986 remote=1001 lr=0.496225 avg_lr=0.496344 avg_rr=0.503656
td 6 frontier=0 time=0.005765 msg_send=0.000000 msg_recv=0.000001 local=1 remote=0 lr=1.000000 avg_lr=1.000000 avg_rr=0.000000
Trial Time:          0.73708
td 0 frontier=4 time=0.005750 msg_send=0.000000 msg_recv=0.000019 local=2 remote=2 lr=0.500000 avg_lr=0.500000 avg_rr=0.500000
td 1 frontier=114798 time=0.005763 msg_send=0.000015 msg_recv=0.000784 local=61488 remote=61702 lr=0.499131 avg_lr=0.496440 avg_rr=0.50356
bu 2 awake=17401801 time=0.428828 u_scanned=33439629 v_checked=50265272 curr_true=17401801
bu 3 awake=4262294 time=0.123176 u_scanned=16037828 v_checked=4277810 curr_true=4262294
bu 4 awake=6097 time=0.010243 u_scanned=11775534 v_checked=8960 curr_true=6097
td 5 frontier=5 time=0.006426 msg_send=0.000005 msg_recv=0.000025 local=3075 remote=3035 lr=0.503273 avg_lr=0.503363 avg_rr=0.496637
td 6 frontier=0 time=0.005623 msg_send=0.000001 msg_recv=0.000018 local=3 remote=2 lr=0.600000 avg_lr=0.583333 avg_rr=0.416667
Trial Time:          0.73688
td 0 frontier=25 time=0.005776 msg_send=0.000000 msg_recv=0.000016 local=17 remote=8 lr=0.680000 avg_lr=0.680000 avg_rr=0.320000
td 1 frontier=622479 time=0.008084 msg_send=0.000080 msg_recv=0.009605 local=438984 remote=436416 lr=0.501467 avg_lr=0.501303 avg_rr=0.498697
bu 2 awake=19476976 time=0.415564 u_scanned=32931927 v_checked=29991445 curr_true=19476976
bu 3 awake=1683733 time=0.071820 u_scanned=13454951 v_checked=1689637 curr_true=1683733
bu 4 awake=1785 time=0.009906 u_scanned=11771218 v_checked=4641 curr_true=1785
td 5 frontier=1 time=0.006075 msg_send=0.000000 msg_recv=0.000024 local=882 remote=904 lr=0.493841 avg_lr=0.493734 avg_rr=0.506266
td 6 frontier=0 time=0.005771 msg_send=0.000000 msg_recv=0.000001 local=1 remote=0 lr=1.000000 avg_lr=1.000000 avg_rr=0.000000
Trial Time:          0.77547
td 0 frontier=2 time=0.005752 msg_send=0.000000 msg_recv=0.000001 local=0 remote=2 lr=0.000000 avg_lr=0.000000 avg_rr=1.000000
td 1 frontier=6867 time=0.005730 msg_send=0.000000 msg_recv=0.000106 local=3542 remote=3419 lr=0.508835 avg_lr=0.508835 avg_rr=0.491165
bu 2 awake=10623204 time=0.437520 u_scanned=33547562 v_checked=162946686 curr_true=10623204
bu 3 awake=11110539 time=0.272003 u_scanned=22924358 v_checked=11262449 curr_true=11110539
bu 4 awake=44351 time=0.012023 u_scanned=11813819 v_checked=47261 curr_true=44351
td 5 frontier=36 time=0.008402 msg_send=0.000014 msg_recv=0.000081 local=22199 remote=22491 lr=0.496733 avg_lr=0.496736 avg_rr=0.503264
td 6 frontier=0 time=0.005789 msg_send=0.000000 msg_recv=0.000024 local=17 remote=19 lr=0.472222 avg_lr=0.475000 avg_rr=0.525000
Trial Time:          0.90705
td 0 frontier=14 time=0.005771 msg_send=0.000000 msg_recv=0.000001 local=9 remote=5 lr=0.642857 avg_lr=0.642857 avg_rr=0.357143
td 1 frontier=32002 time=0.005862 msg_send=0.000016 msg_recv=0.000063 local=17272 remote=16981 lr=0.504248 avg_lr=0.506157 avg_rr=0.493843
bu 2 awake=14705326 time=0.445217 u_scanned=33522415 v_checked=81506772 curr_true=14705326
bu 3 awake=7032699 time=0.181593 u_scanned=18817089 v_checked=7073350 curr_true=7032699
bu 4 awake=14944 time=0.010660 u_scanned=11784390 v_checked=17822 curr_true=14944
td 5 frontier=14 time=0.006680 msg_send=0.000007 msg_recv=0.000035 local=7418 remote=7567 lr=0.495028 avg_lr=0.494995 avg_rr=0.505005
td 6 frontier=0 time=0.005761 msg_send=0.000000 msg_recv=0.000020 local=8 remote=6 lr=0.571429 avg_lr=0.604167 avg_rr=0.395833
Trial Time:          0.81694
td 0 frontier=366 time=0.005758 msg_send=0.000000 msg_recv=0.000001 local=182 remote=184 lr=0.497268 avg_lr=0.497268 avg_rr=0.502732
td 1 frontier=3574933 time=0.684149 msg_send=0.001506 msg_recv=0.028010 local=5427110 remote=5421364 lr=0.500265 avg_lr=0.500137 avg_rr=0.499863
bu 2 awake=17876458 time=0.351656 u_scanned=29979132 v_checked=19563401 curr_true=17876458
bu 3 awake=332927 time=0.027836 u_scanned=12102674 v_checked=336272 curr_true=332927
td 4 frontier=314 time=0.026755 msg_send=0.000048 msg_recv=0.000519 local=174930 remote=175551 lr=0.499114 avg_lr=0.499114 avg_rr=0.500886
td 5 frontier=1 time=0.005834 msg_send=0.000001 msg_recv=0.000021 local=140 remote=175 lr=0.444444 avg_lr=0.448830 avg_rr=0.551170
td 6 frontier=0 time=0.005586 msg_send=0.000000 msg_recv=0.000001 local=1 remote=0 lr=1.000000 avg_lr=1.000000 avg_rr=0.000000
Trial Time:          1.37094
td 0 frontier=39 time=0.005771 msg_send=0.000000 msg_recv=0.000022 local=21 remote=18 lr=0.538462 avg_lr=0.538462 avg_rr=0.461538
td 1 frontier=577576 time=0.022800 msg_send=0.000095 msg_recv=0.004449 local=424595 remote=424470 lr=0.500074 avg_lr=0.499666 avg_rr=0.500334
bu 2 awake=19442746 time=0.439276 u_scanned=32976816 v_checked=30629320 curr_true=19442746
bu 3 awake=1762785 time=0.066031 u_scanned=13534070 v_checked=1768880 curr_true=1762785
bu 4 awake=1851 time=0.009883 u_scanned=11771285 v_checked=4709 curr_true=1851
td 5 frontier=2 time=0.006107 msg_send=0.000000 msg_recv=0.000020 local=912 remote=941 lr=0.492175 avg_lr=0.492153 avg_rr=0.507847
td 6 frontier=0 time=0.005614 msg_send=0.000000 msg_recv=0.000001 local=2 remote=0 lr=1.000000 avg_lr=1.000000 avg_rr=0.000000
Trial Time:          0.69040
td 0 frontier=125 time=0.005781 msg_send=0.000000 msg_recv=0.000001 local=62 remote=63 lr=0.496000 avg_lr=0.496000 avg_rr=0.504000
td 1 frontier=1875476 time=0.230442 msg_send=0.000873 msg_recv=0.010003 local=1992252 remote=1993442 lr=0.499851 avg_lr=0.499905 avg_rr=0.500095
bu 2 awake=19222749 time=0.387588 u_scanned=31678830 v_checked=23066592 curr_true=19222749
bu 3 awake=685999 time=0.035955 u_scanned=12456081 v_checked=689880 curr_true=685999
td 4 frontier=649 time=0.054344 msg_send=0.000094 msg_recv=0.000954 local=379162 remote=380281 lr=0.499263 avg_lr=0.499265 avg_rr=0.500735
td 5 frontier=1 time=0.005819 msg_send=0.000000 msg_recv=0.000020 local=327 remote=323 lr=0.503077 avg_lr=0.502415 avg_rr=0.497585
td 6 frontier=0 time=0.005601 msg_send=0.000001 msg_recv=0.000001 local=1 remote=0 lr=1.000000 avg_lr=1.000000 avg_rr=0.000000
Trial Time:          0.84931
Verification:           PASS
Average Time:        0.84572
```

## mp g25 k50 OMP 8

```bash
root@pxe:~/cxl_profiler# OMP_NUM_THREADS=8 ./gapbs_mp/bfs_mp -g 25 -k 50 --hosts=1 --shm-mb=16384 -n 10 -l -v
ioctl CXLMOCK_IOC_REGISTER_HOSTMapped /dev/dax0.0 at 0x7f246c600000 (len=17179869184)
Using 172 threads for graph generation...
Generate Time:       2.23340
Build Time:          2.00562
Restored 8 threads for BFS execution.
Graph has 33554432 nodes and 1592793612 undirected edges for degree: 47
td 0 frontier=2 0.14003 local=2 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 1 frontier=94458 0.10296 local=95390 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
bu 2 awake=16999837 0.28409 u_scanned=33459971 v_checked=53871932 curr_true=16999837
bu 3 awake=4683540 0.11459 u_scanned=16460134 v_checked=4701680 curr_true=4683540
bu 4 awake=7158 0.02394 u_scanned=11776594 v_checked=10019 curr_true=7158
td 5 frontier=4 0.07765 local=5978 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 6 frontier=0 0.06823 local=4 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
Trial Time:          0.82326
td 0 frontier=4 0.07519 local=4 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 1 frontier=44558 0.07373 local=41108 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
bu 2 awake=15461794 0.32376 u_scanned=33509869 v_checked=70928884 curr_true=15461794
bu 3 awake=6266720 0.11781 u_scanned=18048075 v_checked=6298302 curr_true=6266720
bu 4 awake=11916 0.02223 u_scanned=11781355 v_checked=14783 curr_true=11916
td 5 frontier=7 0.06957 local=9003 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 6 frontier=0 0.06542 local=7 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
Trial Time:          0.75773
td 0 frontier=30 0.07134 local=30 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 1 frontier=544387 0.12605 local=427898 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
bu 2 awake=19398418 0.23967 u_scanned=33010014 v_checked=31140436 curr_true=19398418
bu 3 awake=1840177 0.09349 u_scanned=13611596 v_checked=1846457 curr_true=1840177
td 4 frontier=1986 0.21512 local=661494 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 5 frontier=1 0.08991 local=1923 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 6 frontier=0 0.07167 local=1 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
Trial Time:          0.91841
td 0 frontier=4 0.06881 local=4 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 1 frontier=114798 0.07623 local=115348 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
bu 2 awake=17401801 0.27174 u_scanned=33439629 v_checked=50265272 curr_true=17401801
bu 3 awake=4262294 0.11034 u_scanned=16037828 v_checked=4277810 curr_true=4262294
bu 4 awake=6097 0.02150 u_scanned=11775534 v_checked=8960 curr_true=6097
td 5 frontier=5 0.09051 local=5377 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 6 frontier=0 0.06818 local=5 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
Trial Time:          0.71819
td 0 frontier=25 0.07017 local=25 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 1 frontier=622479 0.14236 local=352332 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
bu 2 awake=19476976 0.22412 u_scanned=32931927 v_checked=29991445 curr_true=19476976
bu 3 awake=1683733 0.09113 u_scanned=13454951 v_checked=1689637 curr_true=1683733
td 4 frontier=1785 0.21244 local=596551 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 5 frontier=1 0.08704 local=1735 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 6 frontier=0 0.07279 local=1 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
Trial Time:          0.91206
td 0 frontier=2 0.07563 local=2 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 1 frontier=6867 0.07061 local=6961 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 2 frontier=10623204 1.72461 local=32029715 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
bu 3 awake=11110539 0.15099 u_scanned=22924358 v_checked=11262449 curr_true=11110539
bu 4 awake=44351 0.02515 u_scanned=11813819 v_checked=47261 curr_true=44351
td 5 frontier=36 0.07537 local=20602 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 6 frontier=0 0.06297 local=36 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
Trial Time:          2.19552
td 0 frontier=14 0.06405 local=14 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 1 frontier=32002 0.06257 local=27269 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
bu 2 awake=14705326 0.34469 u_scanned=33522415 v_checked=81506772 curr_true=14705326
bu 3 awake=7032699 0.12094 u_scanned=18817089 v_checked=7073350 curr_true=7032699
bu 4 awake=14944 0.02219 u_scanned=11784390 v_checked=17822 curr_true=14944
td 5 frontier=14 0.06627 local=10838 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 6 frontier=0 0.06201 local=14 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
Trial Time:          0.75375
td 0 frontier=366 0.06736 local=366 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 1 frontier=3574933 0.31674 local=2866867 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
bu 2 awake=17876458 0.18975 u_scanned=29979132 v_checked=19563401 curr_true=17876458
bu 3 awake=332927 0.05164 u_scanned=12102674 v_checked=336272 curr_true=332927
td 4 frontier=314 0.12250 local=102926 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 5 frontier=1 0.06661 local=314 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 6 frontier=0 0.06474 local=1 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
Trial Time:          0.89035
td 0 frontier=39 0.06391 local=39 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 1 frontier=577576 0.10431 local=410203 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
bu 2 awake=19442746 0.21871 u_scanned=32976816 v_checked=30629320 curr_true=19442746
bu 3 awake=1762785 0.09079 u_scanned=13534070 v_checked=1768880 curr_true=1762785
td 4 frontier=1851 0.18254 local=580439 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 5 frontier=2 0.07078 local=1808 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 6 frontier=0 0.06409 local=2 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
Trial Time:          0.80763
td 0 frontier=125 0.06319 local=125 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 1 frontier=1875476 0.19230 local=1185929 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
bu 2 awake=19222749 0.20563 u_scanned=31678830 v_checked=23066592 curr_true=19222749
bu 3 awake=685999 0.06640 u_scanned=12456081 v_checked=689880 curr_true=685999
td 4 frontier=649 0.14591 local=209150 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 5 frontier=1 0.06854 local=643 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 6 frontier=0 0.06436 local=1 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
Trial Time:          0.81716

Verification:           PASS
Average Time:        0.95941
```


## mp g25 k50 OMP 16

```bash
root@pxe:~/cxl_profiler# OMP_NUM_THREADS=16 ./gapbs_mp/bfs_mp -g 25 -k 50 --hosts=1 --shm-mb=16384 -n 10 -l -v
ioctl CXLMOCK_IOC_REGISTER_HOSTMapped /dev/dax0.0 at 0x7fb0ffe00000 (len=17179869184)
Using 172 threads for graph generation...
Restored 16 threads for BFS execution.
Graph has 33554432 nodes and 1592793612 undirected edges for degree: 47
td 0 frontier=2 0.13333 local=2 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 1 frontier=94458 0.09746 local=95390 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
bu 2 awake=16999837 0.14804 u_scanned=33459971 v_checked=53871932 curr_true=16999837
bu 3 awake=4683540 0.05707 u_scanned=16460134 v_checked=4701680 curr_true=4683540
bu 4 awake=7158 0.01521 u_scanned=11776594 v_checked=10019 curr_true=7158
td 5 frontier=4 0.08732 local=3861 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 6 frontier=0 0.06292 local=4 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
Trial Time:          0.60885
td 0 frontier=4 0.08004 local=4 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 1 frontier=44558 0.07606 local=36450 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
bu 2 awake=15461794 0.16356 u_scanned=33509869 v_checked=70928884 curr_true=15461794
bu 3 awake=6266720 0.07107 u_scanned=18048075 v_checked=6298302 curr_true=6266720
bu 4 awake=11916 0.01309 u_scanned=11781355 v_checked=14783 curr_true=11916
td 5 frontier=7 0.06554 local=3947 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 6 frontier=0 0.06271 local=7 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
Trial Time:          0.53981
td 0 frontier=30 0.08239 local=30 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 1 frontier=544387 0.13903 local=356264 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
bu 2 awake=19398418 0.12043 u_scanned=33010014 v_checked=31140436 curr_true=19398418
bu 3 awake=1840177 0.04365 u_scanned=13611596 v_checked=1846457 curr_true=1840177
td 4 frontier=1986 0.16255 local=346588 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 5 frontier=1 0.08895 local=1845 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 6 frontier=0 0.06652 local=1 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
Trial Time:          0.71076
td 0 frontier=4 0.07934 local=4 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 1 frontier=114798 0.07804 local=99857 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
bu 2 awake=17401801 0.13813 u_scanned=33439629 v_checked=50265272 curr_true=17401801
bu 3 awake=4262294 0.05334 u_scanned=16037828 v_checked=4277810 curr_true=4262294
bu 4 awake=6097 0.01217 u_scanned=11775534 v_checked=8960 curr_true=6097
td 5 frontier=5 0.08582 local=4245 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 6 frontier=0 0.05965 local=5 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
Trial Time:          0.51354
td 0 frontier=25 0.08056 local=25 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 1 frontier=622479 0.10612 local=367580 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
bu 2 awake=19476976 0.11704 u_scanned=32931927 v_checked=29991445 curr_true=19476976
bu 3 awake=1683733 0.04247 u_scanned=13454951 v_checked=1689637 curr_true=1683733
td 4 frontier=1785 0.15844 local=309123 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 5 frontier=1 0.08801 local=1670 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 6 frontier=0 0.06647 local=1 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
Trial Time:          0.66616
td 0 frontier=2 0.07955 local=2 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 1 frontier=6867 0.06263 local=6961 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 2 frontier=10623204 2.10008 local=16239611 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
bu 3 awake=11110539 0.07714 u_scanned=22924358 v_checked=11262449 curr_true=11110539
bu 4 awake=44351 0.01334 u_scanned=11813819 v_checked=47261 curr_true=44351
td 5 frontier=36 0.10083 local=8090 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 6 frontier=0 0.06862 local=36 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
Trial Time:          2.50937
td 0 frontier=14 0.08124 local=14 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 1 frontier=32002 0.06628 local=15191 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
bu 2 awake=14705326 0.17320 u_scanned=33522415 v_checked=81506772 curr_true=14705326
bu 3 awake=7032699 0.07998 u_scanned=18817089 v_checked=7073350 curr_true=7032699
bu 4 awake=14944 0.01287 u_scanned=11784390 v_checked=17822 curr_true=14944
td 5 frontier=14 0.08692 local=4421 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 6 frontier=0 0.06533 local=14 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
Trial Time:          0.57393
td 0 frontier=366 0.06555 local=366 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 1 frontier=3574933 0.36297 local=2212239 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
bu 2 awake=17876458 0.08988 u_scanned=29979132 v_checked=19563401 curr_true=17876458
bu 3 awake=332927 0.02525 u_scanned=12102674 v_checked=336272 curr_true=332927
td 4 frontier=314 0.13737 local=58037 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 5 frontier=1 0.07827 local=311 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 6 frontier=0 0.06918 local=1 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
Trial Time:          0.83564
td 0 frontier=39 0.07883 local=39 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 1 frontier=577576 0.13802 local=338833 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
bu 2 awake=19442746 0.11484 u_scanned=32976816 v_checked=30629320 curr_true=19442746
bu 3 awake=1762785 0.04290 u_scanned=13534070 v_checked=1768880 curr_true=1762785
td 4 frontier=1851 0.16026 local=320078 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 5 frontier=2 0.08732 local=1747 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 6 frontier=0 0.06595 local=2 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
Trial Time:          0.69522
td 0 frontier=125 0.08062 local=125 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 1 frontier=1875476 0.20848 local=883238 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
bu 2 awake=19222749 0.10253 u_scanned=31678830 v_checked=23066592 curr_true=19222749
bu 3 awake=685999 0.03193 u_scanned=12456081 v_checked=689880 curr_true=685999
td 4 frontier=649 0.15065 local=98306 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 5 frontier=1 0.07928 local=638 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
td 6 frontier=0 0.06900 local=1 remote=0 lr=1.00000 avg_lr=1.00000 avg_rr=0.00000
Trial Time:          0.72931
Verification:           PASS
Average Time:        0.83826
```
