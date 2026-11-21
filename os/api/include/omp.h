// omp.h: OpenMP のランタイムの関数（Monocs。os/libc/omp.c）。-fopenmp でコンパイルすると #pragma omp が使える
// （コンパイラの側は cc/omp.inc）。スレッドは pthread（スレッド 1 つに空いたコアを 1 つ）で、チームは
// 「空いたコアの数 + 1（main）」まで（omp_get_max_threads。OMP_NUM_THREADS・omp_set_num_threads で減らせる）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int omp_get_thread_num(void);
int omp_get_num_threads(void);
int omp_get_max_threads(void);
void omp_set_num_threads(int n);
int omp_get_num_procs(void);
int omp_in_parallel(void);
int omp_get_level(void);
int omp_get_dynamic(void);
void omp_set_dynamic(int dynamic);
int omp_get_nested(void);
void omp_set_nested(int nested);
double omp_get_wtime(void);
double omp_get_wtick(void);

typedef struct {
    volatile int locked;
} omp_lock_t;

typedef struct {
    volatile int locked;
    volatile int owner;
    int count;
} omp_nest_lock_t;

void omp_init_lock(omp_lock_t *lock);
void omp_destroy_lock(omp_lock_t *lock);
void omp_set_lock(omp_lock_t *lock);
void omp_unset_lock(omp_lock_t *lock);
int omp_test_lock(omp_lock_t *lock);
void omp_init_nest_lock(omp_nest_lock_t *lock);
void omp_destroy_nest_lock(omp_nest_lock_t *lock);
void omp_set_nest_lock(omp_nest_lock_t *lock);
void omp_unset_nest_lock(omp_nest_lock_t *lock);
int omp_test_nest_lock(omp_nest_lock_t *lock);

#ifdef __cplusplus
}
#endif
