CC ?= gcc

CFLAGS := -std=gnu11 -O3 -march=native -Wall -Wextra -g -pthread -mclflushopt

# SYNC_STATS: sync-event counters (build lib consumers with the same flag)
SYNC_STATS ?= 0
ifeq ($(SYNC_STATS),1)
CFLAGS += -DSYNC_STATS
endif
CFLAGS += -I../global_allocator -Isrc

ALLOC_DIR := ../global_allocator
# NOTE: the lib must be built with the same LOCK_IMPL as this benchmark
# (both default to lfm).
ALLOC_LIB := $(ALLOC_DIR)/build/libglobal_allocator.a

# Static like global_allocator; override with STATIC=0 if static glibc is
# unavailable on the build host.
STATIC ?= 1
ifeq ($(STATIC),1)
LDFLAGS := -static
endif
LDLIBS := -pthread -lm

SRCS := src/ht_bench.c src/shm_ht.c
DEPS := src/shm_ht.h

.PHONY: all clean

all: ht_bench

$(ALLOC_LIB):
	$(MAKE) -C $(ALLOC_DIR) lib

ht_bench: $(SRCS) $(DEPS) $(ALLOC_LIB)
	$(CC) $(CFLAGS) $(SRCS) -o $@ $(ALLOC_LIB) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f ht_bench *.o
