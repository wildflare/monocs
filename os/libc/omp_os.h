// OpenMP のランタイム（os/libc/omp.c）の、コンパイラが生成するコードから呼ばれる関数（cc/omp.inc）
#pragma once

void __monocs_omp_parallel(void (*fn)(void **), void **ctx, int nthreads, int cond);
long long __monocs_omp_trip(long long lb, long long ub, long long st, int op);
void __monocs_omp_loop_init(long long *it, int kind, long long chunk, long long n);
int __monocs_omp_loop_next(long long *it, long long *from, long long *to);
void __monocs_omp_loop_end(long long *it, int nowait);
void __monocs_omp_barrier(void);
int __monocs_omp_single(void);
void __monocs_omp_lock(void);
void __monocs_omp_unlock(void);
void __monocs_omp_flush(void);
