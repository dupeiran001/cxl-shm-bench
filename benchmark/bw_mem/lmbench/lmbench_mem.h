#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define TRIES 80

enum lmbench_mem_t { READ, WRITE, NTWRITE, RDWR, CP, FWR, FRD, FCP, BZERO, BCOPY };

int bw_mem(int parallel, int warmup, int repetitions, uint64_t size, int aligned,
           enum lmbench_mem_t work_type, int numa_node, const char *dax_dev,
           uint64_t dax_offset);
void show_bandwidth(uint64_t time, uint64_t bytes, uint64_t iter,
                    double overhd);
#ifdef __cplusplus
}
#endif
