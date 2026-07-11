#define _GNU_SOURCE
#include <criterion/criterion.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "shm.h"

#define ID_OBJ 0xA11CEULL

static char *make_tmp_path(void) {
  const char *dir = "/dev/shm";
  struct stat st;
  if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
    dir = "/tmp";
  }

  char *tmpl = malloc(256);
  cr_assert_not_null(tmpl);
  snprintf(tmpl, 256, "%s/shm_enable_test_XXXXXX", dir);
  return tmpl;
}

static void prepare_backing_file(const char *path, size_t map_size) {
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
  cr_assert(fd >= 0, "open(%s) failed: %s", path, strerror(errno));

  int rc = ftruncate(fd, (off_t)map_size);
  cr_assert(rc == 0, "ftruncate(%s, %zu) failed: %s", path, map_size,
            strerror(errno));
  close(fd);
}

static void child_host_body(uint32_t host_id, uint32_t host_num,
                            const char *path, size_t map_size) {
  struct shm_arg arg;
  memset(&arg, 0, sizeof(arg));
  arg.host_id = host_id;
  arg.host_num = host_num;
  arg.map_size = map_size;
  snprintf(arg.dax_dev, DEV_NAME_LENGTH, "%s", path);

  /* Only host0 does reset to guarantee fresh state each round. */
  if (host_id == 0) {
    arg.flags = SHM_ENABLE_RESET;
  }

  int32_t rc = shm_enable(arg);
  if (rc != 0) {
    fprintf(stderr, "[host %u] shm_enable failed rc=%d\n", host_id, rc);
    _exit(100);
  }

  /* Small object: 2 cachelines (data + flag), enough to verify visibility. */
  size_t obj_size = 2 * (size_t)CACHE_LINE_SIZE;

  void *p = NULL;

  if (host_id == 0) {
    errno = 0;
    p = shm_malloc_id(ID_OBJ, obj_size);
    if (!p) {
      fprintf(stderr, "[host 0] shm_malloc_id failed errno=%d (%s)\n", errno,
              strerror(errno));
      _exit(101);
    }

    /* Negative: wrong-size attach must fail with EINVAL. */
    errno = 0;
    void *bad = shm_attach_id(ID_OBJ, obj_size + 1);
    if (bad != NULL || errno != EINVAL) {
      fprintf(
          stderr,
          "[host 0] expected wrong-size attach to fail (EINVAL), got bad=%p "
          "errno=%d\n",
          bad, errno);
      _exit(102);
    }

    /* Write a value, publish via cacheline op. */
    volatile uint64_t *w = (volatile uint64_t *)((char *)p + 0);
    STORE_CACHELINE(w, 0x1122334455667788ULL);

    /* Flag == 1 means "ready". */
    volatile uint64_t *flag =
        (volatile uint64_t *)((char *)p + CACHE_LINE_SIZE);
    STORE_CACHELINE(flag, 1);

    /* Wait for host1 to ACK by setting flag to 2. */
    for (;;) {
      uint64_t v = LOAD_CACHELINE(flag);
      if (v == 2)
        break;
      relax_cpu();
    }

    /* Now detach+free must succeed (since host1 detached first). */
    rc = shm_detach_id(ID_OBJ);
    if (rc != 0) {
      fprintf(stderr, "[host 0] detach rc=%d\n", rc);
      _exit(103);
    }

    rc = shm_free_id(ID_OBJ);
    if (rc != 0) {
      fprintf(stderr, "[host 0] free rc=%d\n", rc);
      _exit(104);
    }

    /* After free, attach should fail with ENOENT. */
    errno = 0;
    void *gone = shm_attach_id(ID_OBJ, obj_size);
    if (gone != NULL || errno != ENOENT) {
      fprintf(stderr,
              "[host 0] expected attach-after-free ENOENT, got %p errno=%d\n",
              gone, errno);
      _exit(105);
    }

    _exit(0);
  } else {
    /* Host1: attach will race; retry on ENOENT. */
    for (;;) {
      errno = 0;
      p = shm_attach_id(ID_OBJ, obj_size);
      if (p)
        break;
      if (errno != ENOENT) {
        fprintf(stderr, "[host 1] attach failed errno=%d (%s)\n", errno,
                strerror(errno));
        _exit(110);
      }
      relax_cpu();
    }

    volatile uint64_t *flag =
        (volatile uint64_t *)((char *)p + CACHE_LINE_SIZE);

    /* Wait until host0 ready */
    for (;;) {
      uint64_t v = LOAD_CACHELINE(flag);
      if (v == 1)
        break;
      relax_cpu();
    }

    /* Read data must match */
    volatile uint64_t *w = (volatile uint64_t *)((char *)p + 0);
    uint64_t got = LOAD_CACHELINE(w);
    if (got != 0x1122334455667788ULL) {
      fprintf(stderr, "[host 1] data mismatch got=0x%016llx\n",
              (unsigned long long)got);
      _exit(111);
    }

    /* Detach first, then ACK by writing flag=2 (still ok: object memory exists
       until free). */
    rc = shm_detach_id(ID_OBJ);
    if (rc != 0) {
      fprintf(stderr, "[host 1] detach rc=%d\n", rc);
      _exit(112);
    }

    STORE_CACHELINE(flag, 2);

    _exit(0);
  }
}

static void run_case_two_hosts(const char *path, size_t map_size) {
  pid_t p0 = fork();
  cr_assert(p0 >= 0);
  if (p0 == 0) {
    child_host_body(0, 2, path, map_size);
  }

  pid_t p1 = fork();
  cr_assert(p1 >= 0);
  if (p1 == 0) {
    child_host_body(1, 2, path, map_size);
  }

  int st0 = 0, st1 = 0;
  cr_assert(waitpid(p0, &st0, 0) == p0);
  cr_assert(waitpid(p1, &st1, 0) == p1);

  cr_assert(WIFEXITED(st0) && WEXITSTATUS(st0) == 0, "host0 failed exit=%d",
            WIFEXITED(st0) ? WEXITSTATUS(st0) : -1);
  cr_assert(WIFEXITED(st1) && WEXITSTATUS(st1) == 0, "host1 failed exit=%d",
            WIFEXITED(st1) ? WEXITSTATUS(st1) : -1);
}

Test(shm_enable, fork_2_processes_api_only_validate) {
  size_t map_size = 8UL * 1024 * 1024; /* fine for API test */
  const int rounds = 10;

  char *tmpl = make_tmp_path();
  int fd = mkstemp(tmpl);
  cr_assert(fd >= 0, "mkstemp failed: %s", strerror(errno));
  close(fd);

  for (int r = 0; r < rounds; r++) {
    prepare_backing_file(tmpl, map_size);
    run_case_two_hosts(tmpl, map_size);
  }

  unlink(tmpl);
  free(tmpl);
}

#define ID_OBJ4 0xD00D4ULL

static void child_host_body4(uint32_t host_id, const char *path,
                             size_t map_size) {
  const uint32_t host_num = 4;

  struct shm_arg arg;
  memset(&arg, 0, sizeof(arg));
  arg.host_id = host_id;
  arg.host_num = host_num;
  arg.map_size = map_size;
  snprintf(arg.dax_dev, DEV_NAME_LENGTH, "%s", path);

  if (host_id == 0) {
    arg.flags = SHM_ENABLE_RESET;
  }

  int32_t rc = shm_enable(arg);
  if (rc != 0) {
    fprintf(stderr, "[host %u] shm_enable failed rc=%d\n", host_id, rc);
    _exit(200);
  }

  /*
   * Layout:
   *  CL0: data
   *  CL1: ready (host0 sets to 1 after publishing data)
   *  CL2..CL(2+host_num-1): ack state per host
   *     0 = not seen
   *     1 = seen data / passed check
   *     2 = detached
   */
  const size_t obj_size = (size_t)(2 + host_num) * (size_t)CACHE_LINE_SIZE;

  void *p = NULL;

  if (host_id == 0) {
    errno = 0;
    p = shm_malloc_id(ID_OBJ4, obj_size);
    if (!p) {
      fprintf(stderr, "[host 0] shm_malloc_id failed errno=%d (%s)\n", errno,
              strerror(errno));
      _exit(201);
    }

    volatile uint64_t *data =
        (volatile uint64_t *)((char *)p + 0 * CACHE_LINE_SIZE);
    volatile uint64_t *ready =
        (volatile uint64_t *)((char *)p + 1 * CACHE_LINE_SIZE);

    /* Clear ack flags */
    for (uint32_t h = 0; h < host_num; ++h) {
      volatile uint64_t *ack =
          (volatile uint64_t *)((char *)p + (size_t)(2 + h) * CACHE_LINE_SIZE);
      STORE_CACHELINE(ack, 0);
    }

    /* Publish */
    STORE_CACHELINE(data, 0xAABBCCDDEEFF0011ULL);
    STORE_CACHELINE(ready, 1);

    /* Host0 also marks itself "seen" */
    {
      volatile uint64_t *ack0 =
          (volatile uint64_t *)((char *)p + (size_t)(2 + 0) * CACHE_LINE_SIZE);
      STORE_CACHELINE(ack0, 1);
    }

    /* Phase 1: wait all hosts saw data (ack==1) */
    for (;;) {
      bool all = true;
      for (uint32_t h = 0; h < host_num; ++h) {
        volatile uint64_t *ack =
            (volatile uint64_t *)((char *)p +
                                  (size_t)(2 + h) * CACHE_LINE_SIZE);
        if (LOAD_CACHELINE(ack) < 1) { /* <1 means not seen yet */
          all = false;
          break;
        }
      }
      if (all)
        break;
      relax_cpu();
    }

    /* Now host0 detaches and marks itself detached (ack==2) */
    rc = shm_detach_id(ID_OBJ4);
    if (rc != 0) {
      fprintf(stderr, "[host 0] detach rc=%d\n", rc);
      _exit(202);
    }
    {
      volatile uint64_t *ack0 =
          (volatile uint64_t *)((char *)p + (size_t)(2 + 0) * CACHE_LINE_SIZE);
      STORE_CACHELINE(ack0, 2);
    }

    /* Phase 2: wait all other hosts detached (ack==2) */
    for (;;) {
      bool all = true;
      for (uint32_t h = 0; h < host_num; ++h) {
        volatile uint64_t *ack =
            (volatile uint64_t *)((char *)p +
                                  (size_t)(2 + h) * CACHE_LINE_SIZE);
        if (LOAD_CACHELINE(ack) != 2) {
          all = false;
          break;
        }
      }
      if (all)
        break;
      relax_cpu();
    }

    /* Safe to free now (user_bitmap should be 0) */
    rc = shm_free_id(ID_OBJ4);
    if (rc != 0) {
      fprintf(stderr, "[host 0] free rc=%d\n", rc);
      _exit(203);
    }

    /* After free, attach must fail */
    errno = 0;
    void *gone = shm_attach_id(ID_OBJ4, obj_size);
    if (gone != NULL || errno != ENOENT) {
      fprintf(stderr,
              "[host 0] expected attach-after-free ENOENT, got %p errno=%d\n",
              gone, errno);
      _exit(204);
    }

    _exit(0);
  }

  /* Hosts 1..3: attach retry on ENOENT */
  for (;;) {
    errno = 0;
    p = shm_attach_id(ID_OBJ4, obj_size);
    if (p)
      break;
    if (errno != ENOENT) {
      fprintf(stderr, "[host %u] attach failed errno=%d (%s)\n", host_id, errno,
              strerror(errno));
      _exit(210);
    }
    relax_cpu();
  }

  volatile uint64_t *data =
      (volatile uint64_t *)((char *)p + 0 * CACHE_LINE_SIZE);
  volatile uint64_t *ready =
      (volatile uint64_t *)((char *)p + 1 * CACHE_LINE_SIZE);
  volatile uint64_t *ack =
      (volatile uint64_t *)((char *)p +
                            (size_t)(2 + host_id) * CACHE_LINE_SIZE);

  /* Wait ready */
  for (;;) {
    if (LOAD_CACHELINE(ready) == 1)
      break;
    relax_cpu();
  }

  uint64_t got = LOAD_CACHELINE(data);
  if (got != 0xAABBCCDDEEFF0011ULL) {
    fprintf(stderr, "[host %u] data mismatch got=0x%016llx\n", host_id,
            (unsigned long long)got);
    _exit(211);
  }

  /* Phase 1 ACK: saw data */
  STORE_CACHELINE(ack, 1);

  /* Detach, then Phase 2 ACK: detached */
  rc = shm_detach_id(ID_OBJ4);
  if (rc != 0) {
    fprintf(stderr, "[host %u] detach rc=%d\n", host_id, rc);
    _exit(212);
  }
  STORE_CACHELINE(ack, 2);

  _exit(0);
}

static void run_case_four_hosts(const char *path, size_t map_size) {
  pid_t pids[4] = {0};

  for (uint32_t h = 0; h < 4; ++h) {
    pid_t pid = fork();
    cr_assert(pid >= 0);
    if (pid == 0) {
      child_host_body4(h, path, map_size);
    }
    pids[h] = pid;
  }

  int failures = 0;
  for (uint32_t h = 0; h < 4; ++h) {
    int st = 0;
    cr_assert(waitpid(pids[h], &st, 0) == pids[h]);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
      ++failures;
      if (WIFEXITED(st)) {
        cr_log_error("host %u exited with code %d\n", h, WEXITSTATUS(st));
      } else if (WIFSIGNALED(st)) {
        cr_log_error("host %u killed by signal %d\n", h, WTERMSIG(st));
      } else {
        cr_log_error("host %u exited abnormally\n", h);
      }
    }
  }
  cr_assert(failures == 0, "four-host smoke test had %d failures", failures);
}

Test(shm_enable, fork_4_processes_api_only_smoke) {
  size_t map_size = 16UL * 1024 * 1024;
  const int rounds = 10;

  char *tmpl = make_tmp_path();
  int fd = mkstemp(tmpl);
  cr_assert(fd >= 0, "mkstemp failed: %s", strerror(errno));
  close(fd);

  for (int r = 0; r < rounds; r++) {
    prepare_backing_file(tmpl, map_size);
    run_case_four_hosts(tmpl, map_size);
  }

  unlink(tmpl);
  free(tmpl);
}
