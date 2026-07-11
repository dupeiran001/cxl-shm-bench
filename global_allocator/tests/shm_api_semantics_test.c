// tests/shm_api_semantics_test.c
#define _GNU_SOURCE
#include <criterion/criterion.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "shm.h"

static char *make_tmp_path(void) {
  const char *dir = "/dev/shm";
  struct stat st;
  if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
    dir = "/tmp";
  }

  char *tmpl = malloc(256);
  cr_assert_not_null(tmpl);
  snprintf(tmpl, 256, "%s/shm_api_test_XXXXXX", dir);
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

static void run_single_host_api_sequence(const char *path, size_t map_size) {
  struct shm_arg arg;
  memset(&arg, 0, sizeof(arg));
  arg.host_id = 0;
  arg.host_num = 1;
  arg.map_size = map_size;
  strncpy(arg.dax_dev, path, DEV_NAME_LENGTH - 1);
  arg.dax_dev[DEV_NAME_LENGTH - 1] = '\0';

  int32_t rc = shm_enable(arg);
  cr_assert_eq(rc, 0, "shm_enable failed: %d", rc);

  uint64_t id = 0xCAFEBABEULL;
  size_t size = 4096;

  void *p = NULL;
  void *q = NULL;

  /* 1) attach before allocation -> ENOENT */
  errno = 0;
  p = shm_attach_id(id, size);
  cr_assert_eq(p, NULL);
  cr_assert_eq(errno, ENOENT);

  /* 2) detach before allocation -> -ENOENT */
  rc = shm_detach_id(id);
  cr_assert_eq(rc, -ENOENT);

  /* 3) free before allocation -> -ENOENT */
  rc = shm_free_id(id);
  cr_assert_eq(rc, -ENOENT);

  /* 4) malloc -> success */
  errno = 0;
  p = shm_malloc_id(id, size);
  cr_assert_neq(p, NULL);
  cr_assert_eq(errno, 0);

  /* 5) double malloc same ID -> EEXIST */
  errno = 0;
  q = shm_malloc_id(id, size);
  cr_assert_eq(q, NULL);
  cr_assert_eq(errno, EEXIST);

  /* 6) attach wrong size -> EINVAL */
  errno = 0;
  q = shm_attach_id(id, size + 64);
  cr_assert_eq(q, NULL);
  cr_assert_eq(errno, EINVAL);

  /* 7) attach right size -> success, same base address */
  errno = 0;
  q = shm_attach_id(id, size);
  cr_assert_neq(q, NULL);
  cr_assert_eq(errno, 0);
  /* We don't strictly require pointer equality, but it's expected in this impl.
   */
  cr_assert_eq(q, p, "attach returned different address than malloc");

  /* 8) free while still attached (this host in bitmap) -> -EBUSY */
  rc = shm_free_id(id);
  cr_assert_eq(rc, -EBUSY);

  /* 9) detach twice -> 0 then 0 */
  rc = shm_detach_id(id);
  cr_assert_eq(rc, 0);
  rc = shm_detach_id(id);
  cr_assert_eq(rc, 0);

  /* 10) free after detach -> 0 */
  rc = shm_free_id(id);
  cr_assert_eq(rc, 0);

  /* 11) double free -> -ENOENT */
  rc = shm_free_id(id);
  cr_assert_eq(rc, -ENOENT);
}

Test(shm_api, single_host_all_combinations) {
  size_t map_size = 8UL * 1024 * 1024; /* 8MB is plenty */
  char *tmpl = make_tmp_path();
  int fd = mkstemp(tmpl);
  cr_assert(fd >= 0, "mkstemp failed: %s", strerror(errno));
  close(fd);

  prepare_backing_file(tmpl, map_size);

  run_single_host_api_sequence(tmpl, map_size);

  unlink(tmpl);
  free(tmpl);
}
