CC ?= gcc
AR ?= ar
OBJ_DIR := build
CFLAGS ?= -O3 -march=native -std=gnu11 -Wall -Wextra -pthread -mclflushopt
CFLAGS += -I.
LDFLAGS ?= -static

#############################################################################
# Lock Implementation Selection
# Override via: make LOCK_IMPL={lfm,peterson,bakery}
#############################################################################
LOCK_IMPL ?= lfm
CFLAGS += -DLOCK_IMPL_$(shell echo $(LOCK_IMPL) | tr 'a-z' 'A-Z')

#############################################################################
# Synchronization-event counters (SYNC_STATS, Table VI quantification)
# Build with: make SYNC_STATS=1   (also rebuild consumers with the flag)
#############################################################################
SYNC_STATS ?= 0
ifeq ($(SYNC_STATS),1)
CFLAGS += -DSYNC_STATS
endif

LIB_NAME := $(OBJ_DIR)/libglobal_allocator.a

# Lock implementation source (from locks/ directory)
LOCK_SRC := locks/$(LOCK_IMPL)_lock.c
LOCK_OBJ := $(OBJ_DIR)/$(LOCK_IMPL)_lock.o

UTIL_SRCS := barrier.c sync_stats.c
LIB_SRCS := shm.c
LIB_OBJS := $(addprefix $(OBJ_DIR)/,$(LIB_SRCS:.c=.o) $(UTIL_SRCS:.c=.o)) $(LOCK_OBJ)

# Single cross-host verifier
VERIFY_DIR := verify
VERIFY_TARGET := $(VERIFY_DIR)/verify_shm

# Criterion unit-test binary (local, against a tmpfs fake device)
UNIT_TEST_TARGET := test
TEST_SRCS := tests/shm_enable_test.c tests/multi_host_shm_test.c tests/shm_api_semantics_test.c tests/barrier_test.c tests/common_tests.c tests/cxl_atomic_test.c tests/lock_test.c

CRITERION_CFLAGS := $(shell pkg-config --cflags criterion 2>/dev/null)
CRITERION_LIBS := $(shell pkg-config --libs criterion 2>/dev/null)
CRITERION_LIBS ?= -lcriterion -lm -pthread

TARGETS := $(LIB_NAME) $(UNIT_TEST_TARGET) $(VERIFY_TARGET)

.PHONY: all clean lib verify

all: $(TARGETS)

lib: $(LIB_NAME)
libglobal_allocator.a: $(LIB_NAME)

verify: $(VERIFY_TARGET)

$(OBJ_DIR):
	mkdir -p $@

# Lock object file
$(LOCK_OBJ): $(LOCK_SRC) locks/$(LOCK_IMPL)_lock.h common.h shm_mutex.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/barrier.o: barrier.c barrier.h common.h shm_mutex.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/shm.o: shm.c shm.h common.h shm_mutex.h barrier.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/sync_stats.o: sync_stats.c sync_stats.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB_NAME): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(VERIFY_TARGET): $(VERIFY_DIR)/verify_shm.c $(LIB_NAME)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_NAME) $(LDFLAGS)

$(UNIT_TEST_TARGET): $(TEST_SRCS) $(LIB_NAME)
	$(CC) -O0 -g -std=gnu11 -Wall -Wextra -pthread $(CRITERION_CFLAGS) -I. -DLOCK_IMPL_$(shell echo $(LOCK_IMPL) | tr 'a-z' 'A-Z') -o $@ $(TEST_SRCS) $(LIB_NAME) $(CRITERION_LIBS)

clean:
	rm -rf $(TARGETS) $(LIB_OBJS) $(OBJ_DIR)
