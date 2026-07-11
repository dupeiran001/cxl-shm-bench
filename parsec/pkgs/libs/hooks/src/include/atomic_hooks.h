#ifndef PARSEC_ATOMIC_HOOKS_H
#define PARSEC_ATOMIC_HOOKS_H

#ifndef __ASSEMBLER__

#ifdef __cplusplus
extern "C" {
#endif

void __parsec_atomicstats_op(void);

#ifdef __cplusplus
}
#endif

#if defined(PARSEC_ATOMIC_HOOKS) && !defined(PARSEC_ATOMIC_HOOKS_IMPL)

#define __sync_fetch_and_add(ptr, val)   ({ __parsec_atomicstats_op(); __atomic_fetch_add((ptr), (val), __ATOMIC_SEQ_CST); })

#define __sync_bool_compare_and_swap(ptr, oldv, newv)   ({ __typeof__(*(ptr)) __parsec_expected = (oldv);      __parsec_atomicstats_op();      __atomic_compare_exchange_n((ptr), &__parsec_expected, (newv), 0,                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); })

#define __sync_synchronize()   do { __parsec_atomicstats_op(); __atomic_thread_fence(__ATOMIC_SEQ_CST); } while (0)

#endif

#endif

#endif
