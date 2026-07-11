# Top-level orchestrator. Each artifact has its own Makefile and README;
# this target just chains them in dependency order.
#
# Not built by default:
#   - interleave_dax  (requires root + kernel headers; `make kernel`)
#   - parsec          (requires fetching ~500 MB of inputs; `make parsec`)

.PHONY: all lib kernel benchmark synchronization gapbs_mh hashtable_mh parsec clean help

all: lib benchmark synchronization gapbs_mh hashtable_mh

# A2: userspace shared-memory allocator + primitives (library + cross-host verifier).
lib:
	$(MAKE) -C global_allocator lib verify

# A1: single-DAX interleaving kernel module.
kernel:
	$(MAKE) -C interleave_dax

# A3: bw_mem, tool_memory_latency, rtt (fetches + builds Abseil on first run).
benchmark:
	$(MAKE) -C benchmark

# A4: lock_bench (the synchronization primitives benchmark driver).
synchronization:
	$(MAKE) -C synchronization

# A5: multi-host GAPBS kernels (bfs_mh, pr_mh, pr_spmv_mh, tc_mh).
gapbs_mh: lib
	$(MAKE) -C gapbs_mh

# A7: concurrent hash table on shared CXL memory (ht_bench).
hashtable_mh: lib
	$(MAKE) -C hashtable_mh

# A6: fetch PARSEC input tarballs + build PARSEC with gcc-hooks.
parsec:
	cd parsec && ./setup.sh

clean:
	-$(MAKE) -C global_allocator clean
	-$(MAKE) -C interleave_dax clean
	-$(MAKE) -C benchmark clean
	-$(MAKE) -C synchronization clean
	-$(MAKE) -C gapbs_mh clean
	-$(MAKE) -C hashtable_mh clean

help:
	@echo "Top-level artifact build targets:"
	@echo "  make all              - Build A2, A3, A4, A5, A7 (root not required)"
	@echo "  make lib              - A2: global_allocator library + verifier"
	@echo "  make kernel           - A1: interleave_dax (root + kernel headers)"
	@echo "  make benchmark        - A3: bw_mem / tool_memory_latency / rtt"
	@echo "  make synchronization  - A4: lock_bench"
	@echo "  make gapbs_mh         - A5: multi-host GAPBS (requires lib)"
	@echo "  make hashtable_mh     - A7: concurrent hash table (requires lib)"
	@echo "  make parsec           - A6: fetch + build PARSEC (./parsec/setup.sh)"
	@echo "  make clean            - Clean all artifact builds (incl. fetched third-party)"
