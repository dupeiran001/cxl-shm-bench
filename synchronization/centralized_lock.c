#include "centralized_lock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static inline uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int g_central_debug = 0;

void central_set_debug(int enabled) { g_central_debug = enabled; }

size_t central_layout_bytes(int num_hosts, int num_locks, int channels) {
  size_t slots = (size_t)num_hosts * (size_t)num_locks * (size_t)channels;
  return slots * sizeof(central_slot_t);
}

void central_init_layout(void *slots_base, int num_hosts, int num_locks,
                         int channels) {
  size_t total = central_layout_bytes(num_hosts, num_locks, channels);
  memset(slots_base, 0, total);
  flush_region(slots_base, total);
}

central_layout_t central_attach_layout(void *slots_base, int num_hosts,
                                      int num_locks, int channels) {
  central_layout_t layout;
  layout.hdr = NULL;
  layout.slots = (central_slot_t *)slots_base;
  layout.num_hosts = num_hosts;
  layout.num_locks = num_locks;
  layout.channels = channels;
  return layout;
}

central_slot_t *central_slot_at(central_layout_t *layout, int lock_id,
                                int host_id, int channel) {
  size_t idx = ((size_t)lock_id * (size_t)layout->num_hosts + (size_t)host_id) *
                   (size_t)layout->channels +
               (size_t)channel;
  return &layout->slots[idx];
}

static void central_manager_loop(central_layout_t *layout,
                                 uint64_t duration_ns) {
  int num_locks = layout->num_locks;
  int num_hosts = layout->num_hosts;
  int channels = layout->channels;

  int *owner_host = calloc((size_t)num_locks, sizeof(int));
  int *owner_chan = calloc((size_t)num_locks, sizeof(int));
  uint64_t *owner_seq = calloc((size_t)num_locks, sizeof(uint64_t));
  uint64_t *owner_start = calloc((size_t)num_locks, sizeof(uint64_t));
  uint64_t *rr_idx = calloc((size_t)num_locks, sizeof(uint64_t));

  if (!owner_host || !owner_chan || !owner_seq || !owner_start || !rr_idx) {
    fprintf(stderr, "central_manager_run: allocation failed\n");
    exit(1);
  }
  for (int i = 0; i < num_locks; i++) {
    owner_host[i] = -1;
  }

  printf("Central lock daemon: hosts=%d locks=%d channels=%d\n", num_hosts,
         num_locks, channels);

  uint64_t start_ns = now_ns();
  uint64_t last_log_ns = start_ns;
  while (duration_ns == 0 || now_ns() - start_ns < duration_ns) {
    if (g_central_debug) {
      uint64_t now = now_ns();
      if (now - last_log_ns > 1000000000ull) {
        int owners = 0;
        for (int i = 0; i < num_locks; i++) {
          if (owner_host[i] >= 0) {
            owners++;
          }
        }
        fprintf(stderr,
                "[daemon] heartbeat elapsed=%.2fs owners=%d\n",
                (double)(now - start_ns) / 1e9, owners);
        last_log_ns = now;
      }
    }
    for (int lock_id = 0; lock_id < num_locks; lock_id++) {
      if (owner_host[lock_id] >= 0) {
        central_slot_t *slot =
            central_slot_at(layout, lock_id, owner_host[lock_id],
                            owner_chan[lock_id]);
        if (owner_start[lock_id] != 0 &&
            now_ns() - owner_start[lock_id] > 2000000000ull) {
          if (g_central_debug) {
            fprintf(stderr,
                    "[daemon] timeout lock=%d host=%d chan=%d seq=%lu, reset\n",
                    lock_id, owner_host[lock_id], owner_chan[lock_id],
                    owner_seq[lock_id]);
          }
          CACHELINE_STORE(&slot->req, 0);
          CACHELINE_STORE(&slot->ack, 0);
          CACHELINE_STORE(&slot->rel, 0);
          CACHELINE_STORE(&slot->rel_ack, 0);
          owner_host[lock_id] = -1;
          owner_chan[lock_id] = 0;
          owner_seq[lock_id] = 0;
          owner_start[lock_id] = 0;
          continue;
        }
        uint64_t rel = CACHELINE_LOAD(&slot->rel);
        if (rel == owner_seq[lock_id]) {
          CACHELINE_STORE(&slot->rel_ack, owner_seq[lock_id]);
          owner_host[lock_id] = -1;
          owner_chan[lock_id] = 0;
          owner_seq[lock_id] = 0;
          owner_start[lock_id] = 0;
        }
        continue;
      }

      uint64_t start = rr_idx[lock_id] %
                       (uint64_t)(num_hosts * channels);
      int granted = 0;
      for (int offset = 0; offset < num_hosts * channels; offset++) {
        uint64_t idx = (start + (uint64_t)offset) %
                       (uint64_t)(num_hosts * channels);
        int host = (int)(idx / (uint64_t)channels);
        int chan = (int)(idx % (uint64_t)channels);
        central_slot_t *slot = central_slot_at(layout, lock_id, host, chan);
        uint64_t seq = CACHELINE_LOAD(&slot->req);
        if (seq == 0) {
          continue;
        }
        if (CACHELINE_LOAD(&slot->ack) == seq) {
          continue;
        }
        CACHELINE_STORE(&slot->ack, seq);
        owner_host[lock_id] = host;
        owner_chan[lock_id] = chan;
        owner_seq[lock_id] = seq;
        owner_start[lock_id] = now_ns();
        rr_idx[lock_id] = idx + 1;
        granted = 1;
        break;
      }
      if (!granted) {
        rr_idx[lock_id] = start + 1;
      }
    }
  }
}

void central_manager_run(central_layout_t *layout) {
  central_manager_loop(layout, 0);
}

void central_manager_run_for(central_layout_t *layout, uint64_t duration_ns) {
  central_manager_loop(layout, duration_ns);
}
int central_local_init(central_local_t *local, int num_locks, int channels,
                       int threads) {
  local->num_locks = num_locks;
  local->channels = channels;
  local->threads = threads;
  local->slot_in_use =
      calloc((size_t)num_locks * (size_t)channels, sizeof(atomic_uint));
  local->next_slot = calloc((size_t)num_locks, sizeof(atomic_uint));
  local->thread_seq = calloc((size_t)threads, sizeof(uint64_t));
  if (!local->slot_in_use || !local->next_slot || !local->thread_seq) {
    return -1;
  }
  return 0;
}

void central_local_destroy(central_local_t *local) {
  free(local->slot_in_use);
  free(local->next_slot);
  free(local->thread_seq);
}

static int claim_slot(central_local_t *local, int lock_id, uint64_t *retries) {
  int channels = local->channels;
  if (channels <= 1) {
    unsigned expected = 0;
    if (atomic_compare_exchange_strong(
            &local->slot_in_use[lock_id * channels], &expected, 1)) {
      return 0;
    }
  }

  while (1) {
    unsigned start =
        atomic_fetch_add(&local->next_slot[lock_id], 1U) % (unsigned)channels;
    for (int i = 0; i < channels; i++) {
      int slot = (start + i) % channels;
      unsigned expected = 0;
      if (atomic_compare_exchange_strong(
              &local->slot_in_use[lock_id * channels + slot], &expected, 1)) {
        return slot;
      }
      if (retries) {
        (*retries)++;
      }
    }
    relax_cpu();
  }
}

int central_acquire(central_layout_t *layout, central_local_t *local, int host_id,
                    int thread_id, int lock_id, central_handle_t *handle,
                    uint64_t *poll_loops, uint64_t *retries) {
  if (!layout || !local || !handle) {
    return -1;
  }
  while (1) {
    uint64_t local_retries = 0;
    int channel = claim_slot(local, lock_id, &local_retries);
    if (retries) {
      *retries += local_retries;
    }

    central_slot_t *slot = central_slot_at(layout, lock_id, host_id, channel);
    CACHELINE_STORE(&slot->req, 0);
    CACHELINE_STORE(&slot->ack, 0);
    CACHELINE_STORE(&slot->rel, 0);
    CACHELINE_STORE(&slot->rel_ack, 0);
    uint64_t last_log = now_ns();
    uint64_t start_wait = last_log;
    while (CACHELINE_LOAD(&slot->req) != 0 ||
           CACHELINE_LOAD(&slot->ack) != 0 ||
           CACHELINE_LOAD(&slot->rel) != 0 ||
           CACHELINE_LOAD(&slot->rel_ack) != 0) {
      if (poll_loops) {
        (*poll_loops)++;
      }
      if (now_ns() - start_wait > 2000000000ull) {
        if (g_central_debug) {
          fprintf(stderr,
                  "[central] host=%d lock=%d chan=%d clear-timeout\n",
                  host_id, lock_id, channel);
        }
        CACHELINE_STORE(&slot->req, 0);
        CACHELINE_STORE(&slot->ack, 0);
        CACHELINE_STORE(&slot->rel, 0);
        CACHELINE_STORE(&slot->rel_ack, 0);
        atomic_store(&local->slot_in_use[lock_id * local->channels + channel],
                     0);
        goto retry;
      }
      if (g_central_debug) {
        uint64_t now = now_ns();
        if (now - last_log > 1000000000ull) {
          fprintf(stderr,
                  "[central] host=%d lock=%d chan=%d waiting-clear "
                  "req=%lu ack=%lu rel=%lu rel_ack=%lu\n",
                  host_id, lock_id, channel, CACHELINE_LOAD(&slot->req),
                  CACHELINE_LOAD(&slot->ack), CACHELINE_LOAD(&slot->rel),
                  CACHELINE_LOAD(&slot->rel_ack));
          last_log = now;
        }
      }
      relax_cpu();
    }

    uint64_t seq = ++local->thread_seq[thread_id];
    CACHELINE_STORE(&slot->req, seq);
    start_wait = now_ns();
    while (CACHELINE_LOAD(&slot->ack) != seq) {
      if (poll_loops) {
        (*poll_loops)++;
      }
      if (now_ns() - start_wait > 2000000000ull) {
        if (g_central_debug) {
          fprintf(stderr,
                  "[central] host=%d lock=%d chan=%d ack-timeout seq=%lu\n",
                  host_id, lock_id, channel, seq);
        }
        CACHELINE_STORE(&slot->req, 0);
        CACHELINE_STORE(&slot->ack, 0);
        CACHELINE_STORE(&slot->rel, 0);
        CACHELINE_STORE(&slot->rel_ack, 0);
        atomic_store(&local->slot_in_use[lock_id * local->channels + channel],
                     0);
        goto retry;
      }
      if (g_central_debug) {
        uint64_t now = now_ns();
        if (now - last_log > 1000000000ull) {
          fprintf(stderr,
                  "[central] host=%d lock=%d chan=%d waiting-ack "
                  "seq=%lu ack=%lu\n",
                  host_id, lock_id, channel, seq,
                  CACHELINE_LOAD(&slot->ack));
          last_log = now;
        }
      }
      relax_cpu();
    }

    handle->lock_id = lock_id;
    handle->channel = channel;
    handle->seq = seq;
    return 0;
  retry:
    continue;
  }
}

int central_release(central_layout_t *layout, central_local_t *local, int host_id,
                    central_handle_t *handle, uint64_t *poll_loops) {
  if (!layout || !local || !handle) {
    return -1;
  }
  central_slot_t *slot =
      central_slot_at(layout, handle->lock_id, host_id, handle->channel);
  CACHELINE_STORE(&slot->rel, handle->seq);

  uint64_t last_log = now_ns();
  while (CACHELINE_LOAD(&slot->rel_ack) != handle->seq) {
    if (poll_loops) {
      (*poll_loops)++;
    }
    if (g_central_debug) {
      uint64_t now = now_ns();
      if (now - last_log > 1000000000ull) {
        fprintf(stderr,
                "[central] host=%d lock=%d chan=%d waiting-rel_ack "
                "seq=%lu rel_ack=%lu\n",
                host_id, handle->lock_id, handle->channel, handle->seq,
                CACHELINE_LOAD(&slot->rel_ack));
        last_log = now;
      }
    }
    relax_cpu();
  }

  CACHELINE_STORE(&slot->req, 0);
  CACHELINE_STORE(&slot->ack, 0);
  CACHELINE_STORE(&slot->rel, 0);
  CACHELINE_STORE(&slot->rel_ack, 0);

  atomic_store(&local->slot_in_use[handle->lock_id * local->channels +
                                  handle->channel],
               0);
  return 0;
}
