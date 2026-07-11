// sync_stats.c - counter storage + dump for -DSYNC_STATS builds (SYNC_STATS)
#ifdef SYNC_STATS

#include "sync_stats.h"
#include <stdio.h>
#include <string.h>

sync_stats_t g_sync_stats;

void sync_stats_reset(void) { memset(&g_sync_stats, 0, sizeof(g_sync_stats)); }

void sync_stats_dump(const char *tag) {
  // One line, machine-readable; consumed by gapbs_mh/scripts/sync_profile.py
  printf("SYNC_STATS tag=%s publish_lines=%llu observe_lines=%llu "
         "lock_acquires=%llu lock_retries=%llu barrier_waits=%llu "
         "atomic_rmw=%llu\n",
         tag ? tag : "-",
         (unsigned long long)g_sync_stats.publish_lines,
         (unsigned long long)g_sync_stats.observe_lines,
         (unsigned long long)g_sync_stats.lock_acquires,
         (unsigned long long)g_sync_stats.lock_retries,
         (unsigned long long)g_sync_stats.barrier_waits,
         (unsigned long long)g_sync_stats.atomic_rmw);
  fflush(stdout);
}

#endif /* SYNC_STATS */
