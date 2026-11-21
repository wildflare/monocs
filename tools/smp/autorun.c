// 実機でのマルチコアの確認用（make sdimage-smp-auto が /autorun.c として入れる）
// UART がなくても確かめられるよう、結果を ACT LED で知らせる:
//   確認中: 点灯したまま
//   成功: 「ピカッ」を 4 回、休み、の繰り返し
//   失敗: 速い点滅を 2 秒、そのあと起動したコアの数だけ「ピカッ」、休み、の繰り返し
// 確かめること: 4 コアが起動するか、分担した計算の結果が 1 コアと同じか、
// 4 コアが同じ変数をアトミック命令で増やした合計が正しいか（コア間のキャッシュの一貫性）
#include <stdatomic.h>
#include <stdbool.h>
#include <monocs.h>

#define LIMIT 300000
#define INCS 1000000

struct job {
    int from, to, count;
};

static atomic_int shared;
static volatile int start_flag;

static int is_prime(int n)
{
    if (n < 2)
        return 0;
    if (n % 2 == 0)
        return n == 2;
    for (int d = 3; d * d <= n; d += 2)
        if (n % d == 0)
            return 0;
    return 1;
}

static void count_range(void *arg)
{
    struct job *j = arg;
    int c = 0;
    for (int n = j->from; n < j->to; n++)
        c += is_prime(n);
    j->count = c;
}

static void increment(void *arg)
{
    (void)arg;
    while (!start_flag)
        ;
    for (int i = 0; i < INCS; i++)
        atomic_fetch_add(&shared, 1);
}

static void flash(int times, unsigned on_us, unsigned off_us)
{
    for (int i = 0; i < times; i++) {
        led_set(true);
        timer_delay_us(on_us);
        led_set(false);
        timer_delay_us(off_us);
    }
}

int main(void)
{
    led_set(true);
    int cores = monocs_cores();
    bool ok = cores == 4;

    // 1 コアで数える
    struct job one = {0, LIMIT, 0};
    count_range(&one);

    // 4 コアで分担する
    if (ok) {
        struct job jobs[4];
        for (int i = 0; i < 4; i++) {
            jobs[i].from = i * (LIMIT / 4);
            jobs[i].to = i == 3 ? LIMIT : (i + 1) * (LIMIT / 4);
            jobs[i].count = 0;
        }
        for (int i = 1; i < 4; i++)
            ok &= monocs_core_start(i, count_range, &jobs[i]);
        count_range(&jobs[0]);
        int total = jobs[0].count;
        for (int i = 1; i < 4; i++) {
            ok &= monocs_core_wait(i);
            total += jobs[i].count;
        }
        ok &= total == one.count;
    }

    // 4 コアで同じ変数を増やす
    if (ok) {
        atomic_store(&shared, 0);
        start_flag = 0;
        for (int i = 1; i < 4; i++)
            ok &= monocs_core_start(i, increment, 0);
        start_flag = 1;
        increment(0);
        for (int i = 1; i < 4; i++)
            ok &= monocs_core_wait(i);
        ok &= atomic_load(&shared) == 4 * INCS;
    }

    for (;;) {
        if (ok) {
            flash(4, 150000, 150000);
            timer_delay_us(1200000);
        } else {
            flash(10, 100000, 100000);
            timer_delay_us(500000);
            flash(cores, 400000, 400000);
            timer_delay_us(1500000);
        }
    }
}
