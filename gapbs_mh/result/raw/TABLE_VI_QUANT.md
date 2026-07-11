# Table VI quantification — measured synchronization rates

First-order sync share = (locks x 3.5us + atomics x 3.0us + barriers x 1000us) / runtime.

| kernel | config | hosts | time s | lock/s | atomic/s | barrier/s | publish lines/s | observe lines/s | est. sync share |
|---|---|---|---|---|---|---|---|---|---|
| bfs | shared_2hosts | 2 | 1.573 | 73 | 0 | 36.9 | 1,150,656,446 | 29,186,401 | 1.9% |
| bfs | shared_4hosts | 4 | 0.835 | 232 | 0 | 139.0 | 4,150,071,451 | 61,807,762 | 3.5% |
| pr | shared_2hosts | 2 | 13.270 | 8 | 0 | 4.7 | 39,489,699 | 819,038 | 0.2% |
| pr | shared_4hosts | 4 | 10.673 | 22 | 0 | 16.9 | 100,560,524 | 1,432,735 | 0.4% |
| pr_spmv | shared_2hosts | 2 | 7.140 | 13 | 0 | 7.3 | 65,756,776 | 1,649,592 | 0.4% |
| pr_spmv | shared_4hosts | 4 | 4.893 | 37 | 0 | 26.2 | 190,206,522 | 2,948,131 | 0.7% |
| tc | shared_2hosts | 2 | 3.075 | 13 | 0 | 3.9 | 3,834,901 | 1,616,342 | 0.2% |
| tc | shared_4hosts | 4 | 2.043 | 29 | 0 | 11.7 | 11,536,376 | 4,507,562 | 0.3% |
