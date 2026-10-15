CXX ?= g++

CXX_FLAGS := -std=c++11 -O3 -Wall -fopenmp -pthread -g -mclflushopt
CXX_FLAGS += -I../gapbs/src -I../global_allocator -Isrc

# SYNC_STATS counters: must come AFTER the CXX_FLAGS := assignment above
SYNC_STATS ?= 0
ifeq ($(SYNC_STATS),1)
CXX_FLAGS += -DSYNC_STATS
endif

LDFLAGS := -L../global_allocator/build -lglobal_allocator -fopenmp -pthread

COMMON_DEPS := src/shm_support.h src/shm_graph.h src/cxl_util.h

.PHONY: all clean

all: bfs_mh pr_mh pr_spmv_mh tc_mh

bfs_mh: src/bfs_mh.cc $(COMMON_DEPS)
	$(CXX) $(CXX_FLAGS) src/bfs_mh.cc -o $@ $(LDFLAGS)

# pr_mh uses std::atomic_ref (C++20); the other kernels keep -std=c++11.
pr_mh: CXX_FLAGS += -std=c++20
pr_mh: src/pr_mh.cc $(COMMON_DEPS)
	$(CXX) $(CXX_FLAGS) src/pr_mh.cc -o $@ $(LDFLAGS)

pr_spmv_mh: src/pr_spmv_mh.cc $(COMMON_DEPS)
	$(CXX) $(CXX_FLAGS) src/pr_spmv_mh.cc -o $@ $(LDFLAGS)

tc_mh: src/tc_mh.cc $(COMMON_DEPS)
	$(CXX) $(CXX_FLAGS) src/tc_mh.cc -o $@ $(LDFLAGS)

clean:
	rm -f bfs_mh pr_mh pr_spmv_mh tc_mh *.o
