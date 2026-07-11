ROOT := $(abspath $(CURDIR))
BUILD_DIR := $(ROOT)/build
BW_MEM_DIR := $(ROOT)/bw_mem
BW_MEM_LMBENCH_DIR := $(BW_MEM_DIR)/lmbench
TOOL_LAT_DIR := $(ROOT)/tool_memory_latency
UTIL_DIR := $(ROOT)/util

# Abseil is fetched on first build into third_party/abseil-cpp and built
# into third_party/abseil-cpp/build/install. Pin to a specific upstream
# LTS tag. Override ABSL_PREFIX to point at a system install instead.
ABSL_VERSION ?= 20250814.1
ABSL_URL     ?= https://github.com/abseil/abseil-cpp.git
THIRD_PARTY  := $(ROOT)/third_party
ABSL_SRC     := $(THIRD_PARTY)/abseil-cpp
ABSL_BUILD   := $(ABSL_SRC)/build
ABSL_PREFIX  ?= $(ABSL_BUILD)/install
ABSL_MARKER  := $(ABSL_PREFIX)/lib/libabsl_base.a

CC  ?= gcc
CXX ?= g++
ARCH ?= native

ABSL_INCLUDE ?= $(ABSL_PREFIX)/include
ABSL_LIBDIR  ?= $(ABSL_PREFIX)/lib
ABSL_CFLAGS  ?= -I$(ABSL_INCLUDE)

# Lazy expansion: resolve at recipe time (after the marker has been built).
ABSL_LIBS = $(sort $(wildcard $(ABSL_LIBDIR)/libabsl_*.a))

NUMA_STATIC := $(firstword $(wildcard /usr/lib/libnuma.a /usr/lib64/libnuma.a /usr/lib/*/libnuma.a /usr/local/lib/libnuma.a /usr/local/lib/*/libnuma.a))
# libnuma is only needed for bw_mem + tool_memory_latency. rtt, clean, and
# dependency-fetching targets do not use it.
NUMA_SKIP_GOALS := clean deps abseil-clean rtt
ifneq ($(filter $(NUMA_SKIP_GOALS),$(MAKECMDGOALS)),$(MAKECMDGOALS))
ifeq ($(NUMA_STATIC),)
$(error Static libnuma not found. Install the static variant of your distro's libnuma package (providing libnuma.a) and re-run make.)
endif
endif
NUMA_LIBS := $(NUMA_STATIC)

THREAD_FLAGS := -pthread
CPPFLAGS += -D_DEFAULT_SOURCE -D_GNU_SOURCE -I$(ROOT) -I$(BW_MEM_DIR) $(ABSL_CFLAGS)
CFLAGS   += -O2 -g -std=c11 -march=$(ARCH) $(THREAD_FLAGS)
CXXFLAGS += -O2 -g -std=c++20 -march=$(ARCH) $(THREAD_FLAGS)
LDFLAGS  += -static -static-libstdc++ -static-libgcc $(THREAD_FLAGS)

# --start-group / --end-group lets the linker resolve circular deps
# among the Abseil static libs.
LDLIBS_TAIL = -Wl,--start-group $(ABSL_LIBS) -Wl,--end-group $(NUMA_LIBS) -ldl -lrt

BW_MEM_C_SRCS := \
	$(BW_MEM_LMBENCH_DIR)/bw_mem.c \
	$(BW_MEM_LMBENCH_DIR)/getopt.c \
	$(BW_MEM_LMBENCH_DIR)/lib_sched.c \
	$(BW_MEM_LMBENCH_DIR)/lib_timing.c
BW_MEM_CXX_SRCS := \
	$(BW_MEM_LMBENCH_DIR)/call_memory_test.cc \
	$(UTIL_DIR)/init.cc

TOOL_LAT_CXX_SRCS := \
	$(TOOL_LAT_DIR)/memory_lat.cc \
	$(UTIL_DIR)/init.cc \
	$(UTIL_DIR)/perf.cc

BW_MEM_OBJS := $(patsubst $(ROOT)/%, $(BUILD_DIR)/%, $(BW_MEM_C_SRCS:.c=.o) $(BW_MEM_CXX_SRCS:.cc=.o))
TOOL_LAT_OBJS := $(patsubst $(ROOT)/%, $(BUILD_DIR)/%, $(TOOL_LAT_CXX_SRCS:.cc=.o))

BW_MEM_BIN := $(BW_MEM_DIR)/bw_mem
TOOL_LAT_BIN := $(TOOL_LAT_DIR)/tool_memory_latency
RTT_DIR := $(ROOT)/rtt
RTT_BIN := $(RTT_DIR)/rtt

.PHONY: all clean deps abseil abseil-clean rtt

all: $(BW_MEM_BIN) $(TOOL_LAT_BIN) rtt

# rtt has its own self-contained Makefile (no Abseil / libnuma dep).
rtt:
	$(MAKE) -C $(RTT_DIR)

# Object files depend on Abseil being installed (order-only: don't rebuild
# objects when the marker's timestamp updates).
$(BW_MEM_OBJS): | $(ABSL_MARKER)
$(TOOL_LAT_OBJS): | $(ABSL_MARKER)

$(BW_MEM_BIN): $(BW_MEM_OBJS) | $(ABSL_MARKER)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS_TAIL)

$(TOOL_LAT_BIN): $(TOOL_LAT_OBJS) | $(ABSL_MARKER)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS_TAIL)

$(BUILD_DIR)/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(ROOT)/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

# ---------------------------------------------------------------------------
# Abseil: fetch + build on first use.
# Requires git and cmake >= 3.16 on the build host.
# ---------------------------------------------------------------------------

deps: $(ABSL_MARKER)
abseil: $(ABSL_MARKER)

$(ABSL_MARKER):
	@echo "[deps] fetching Abseil $(ABSL_VERSION) from $(ABSL_URL)"
	@rm -rf $(ABSL_SRC)
	@mkdir -p $(THIRD_PARTY)
	git clone --depth 1 --branch $(ABSL_VERSION) $(ABSL_URL) $(ABSL_SRC)
	@echo "[deps] building Abseil $(ABSL_VERSION) (static, Release)"
	cmake -S $(ABSL_SRC) -B $(ABSL_BUILD) \
	    -DCMAKE_BUILD_TYPE=Release \
	    -DCMAKE_INSTALL_PREFIX=$(ABSL_PREFIX) \
	    -DCMAKE_CXX_STANDARD=20 \
	    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	    -DABSL_ENABLE_INSTALL=ON \
	    -DABSL_PROPAGATE_CXX_STD=ON \
	    -DBUILD_SHARED_LIBS=OFF \
	    -DABSL_BUILD_TESTING=OFF
	cmake --build $(ABSL_BUILD) --parallel
	cmake --install $(ABSL_BUILD)

abseil-clean:
	rm -rf $(ABSL_SRC)

# `make clean` removes our build output AND the fetched third-party
# Abseil tree. If you want to keep Abseil cached (and avoid the
# ~5-min re-fetch+rebuild on the next `make`), use `make -C ... clean
# ABSEIL_KEEP=1` or delete individual targets manually.
clean:
	rm -rf $(BUILD_DIR) $(BW_MEM_BIN) $(TOOL_LAT_BIN)
	$(MAKE) -C $(RTT_DIR) clean
	rm -rf $(THIRD_PARTY)
