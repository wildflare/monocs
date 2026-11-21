// top: CPU（コアごと）・メモリ・サービスの様子を見る（Linux の top に似せたもの。monocs_sysstat）
//   top [-b] [-n 回数] [-d 秒]
//     -b       文字だけで出す（画面を書き直さない。ssh のコマンド・パイプ用。端末でなければ自動で）
//     -n 回数  その回数だけ出して終わる     -d 秒  書き直す間（既定 1。0.1 まで）
//   画面では q で終わる。Ctrl-C でも終わる
// Monocs にはプロセスがないので、プロセスの一覧の代わりに「コアごとに何が動いているか」と「サービスごとの CPU」を出す。
// 最初の 1 回の %CPU は、起動してからの平均（Linux の top と同じ）。2 回目からは前の回からの間の値。
#include <curses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <monocs.h>

static MonocsSysStat prev, cur;
static bool have_prev;

static double busy_pct(int c)
{
    uint64_t dt = cur.uptime_us - (have_prev ? prev.uptime_us : 0);
    uint64_t di = cur.core[c].idle_us - (have_prev ? prev.core[c].idle_us : 0);
    if (dt == 0)
        return 0;
    double p = 100.0 * (1.0 - (double)di / (double)dt);
    return p < 0 ? 0 : p > 100 ? 100 : p;
}

static double svc_pct(int i)
{
    uint64_t dt = cur.uptime_us - (have_prev ? prev.uptime_us : 0);
    uint64_t before = 0;
    if (have_prev)
        for (int k = 0; k < prev.nservices; k++)
            if (!strcmp(prev.service[k].name, cur.service[i].name))
                before = prev.service[k].busy_us;
    if (dt == 0 || cur.service[i].busy_us < before)
        return 0;
    double p = 100.0 * (double)(cur.service[i].busy_us - before) / (double)dt;
    return p > 100 ? 100 : p;
}

static const char *core_what(int c, char *buf, size_t size)
{
    switch (cur.core[c].state) {
    case MONOCS_CORE_OFF: return "off";
    case MONOCS_CORE_IDLE: return "idle";
    case MONOCS_CORE_SHELL: return "shell";
    case MONOCS_CORE_APP: snprintf(buf, size, "app %s", cur.app); return buf;
    case MONOCS_CORE_JOB: snprintf(buf, size, "job of %s", cur.app[0] ? cur.app : "app"); return buf;
    case MONOCS_CORE_THREAD: snprintf(buf, size, "thread of %s", cur.app[0] ? cur.app : "app"); return buf;
    case MONOCS_CORE_SERVICES: return "services";
    }
    return "?";
}

// 1 回分を、行ごとに emit で出す
static void frame(void (*emit)(const char *line))
{
    char line[160], what[64];
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    uint64_t up = cur.uptime_us / 1000000;
    const char *power = cur.throttled & 0x1 ? "UNDERVOLTAGE" : cur.throttled & 0x4 ? "throttled"
                      : cur.throttled & 0x50005 ? "had problems" : "ok";
    snprintf(line, sizeof line, "top - %02d:%02d:%02d up %llu:%02llu:%02llu,  %.1f C,  ARM %u MHz,  power %s",
             tm.tm_hour, tm.tm_min, tm.tm_sec, (unsigned long long)(up / 3600), (unsigned long long)(up / 60 % 60),
             (unsigned long long)(up % 60), cur.temp_mc / 1000.0, (unsigned)(cur.arm_hz / 1000000), power);
    emit(line);
    for (int c = 0; c < MONOCS_STAT_CORES; c++) {
        if (cur.core[c].state == MONOCS_CORE_OFF)
            snprintf(line, sizeof line, "%%Cpu%d:     -        off", c);
        else {
            double secs = (double)(cur.uptime_us - (have_prev ? prev.uptime_us : 0)) / 1e6;
            double irqs = (double)(cur.core[c].irqs - (have_prev ? prev.core[c].irqs : 0)) / (secs > 0 ? secs : 1);
            snprintf(line, sizeof line, "%%Cpu%d: %5.1f busy  %6.0f irq/s  %s", c, busy_pct(c), irqs,
                     core_what(c, what, sizeof what));
        }
        emit(line);
    }
    double mib = 1024.0 * 1024.0;
    uint64_t in_use = cur.kernel_in_use + cur.app_in_use;
    uint64_t used = in_use > cur.file_cache ? in_use - cur.file_cache : 0;
    uint64_t free_ = cur.mem_total > in_use ? cur.mem_total - in_use : 0;
    snprintf(line, sizeof line, "MiB Mem : %7.1f total, %7.1f free, %7.1f used, %7.1f buff/cache",
             cur.mem_total / mib, free_ / mib, used / mib, cur.file_cache / mib);
    emit(line);
    snprintf(line, sizeof line, "MiB Heap: kernel %.1f (in use %.1f), app %.1f (in use %.1f)", cur.kernel_heap / mib,
             cur.kernel_in_use / mib, cur.app_heap / mib, cur.app_in_use / mib);
    emit(line);
    emit("");
    // POLL/s: service_poll を呼んだ回数、WORK/s: そのうち「仕事をした」と返した回数（いつも仕事をしたと返すサービスが
    // あると、サービスのコアは眠れない）。SLEEP/s: サービスのコアが眠った回数
    double secs = (double)(cur.uptime_us - (have_prev ? prev.uptime_us : 0)) / 1e6;
    if (secs <= 0)
        secs = 1;
    snprintf(line, sizeof line, "SERVICE (core 3)     %%CPU       TIME+    POLL/s    WORK/s   (core 3 sleeps %.0f/s)",
             (double)(cur.svc_sleeps - (have_prev ? prev.svc_sleeps : 0)) / secs);
    emit(line);
    for (int i = 0; i < cur.nservices; i++) {
        uint64_t cs = cur.service[i].busy_us / 10000;      // 1/100 秒
        uint64_t p0 = 0, w0 = 0;
        if (have_prev)
            for (int k = 0; k < prev.nservices; k++)
                if (!strcmp(prev.service[k].name, cur.service[i].name)) {
                    p0 = prev.service[k].polls;
                    w0 = prev.service[k].work_polls;
                }
        snprintf(line, sizeof line, "%-18s %6.1f %5llu:%02llu.%02llu %9.0f %9.0f", cur.service[i].name, svc_pct(i),
                 (unsigned long long)(cs / 6000), (unsigned long long)(cs / 100 % 60), (unsigned long long)(cs % 100),
                 (double)(cur.service[i].polls - p0) / secs, (double)(cur.service[i].work_polls - w0) / secs);
        emit(line);
    }
    if (cur.nservices == 0)
        emit("(none)");
}

static void emit_stdout(const char *line)
{
    puts(line);
}

static int row;

static void emit_screen(const char *line)
{
    bool bold = row == 0 || !strncmp(line, "SERVICE", 7);
    if (bold)
        attron(A_BOLD);
    mvprintw(row, 0, "%.*s", COLS > 0 ? COLS - 1 : 79, line);
    if (bold)
        attroff(A_BOLD);
    clrtoeol();
    row++;
}

static void sample(void)
{
    if (have_prev)
        prev = cur;
    monocs_sysstat(&cur);
}

int main(int argc, char **argv)
{
    bool batch = !isatty(1);
    long count = -1;
    double delay = 1.0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b"))
            batch = true;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc)
            count = atol(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc)
            delay = atof(argv[++i]);
        else if (!strncmp(argv[i], "-n", 2) && argv[i][2])
            count = atol(argv[i] + 2);
        else if (!strncmp(argv[i], "-d", 2) && argv[i][2])
            delay = atof(argv[i] + 2);
        else {
            fprintf(stderr, "usage: top [-b] [-n COUNT] [-d SECONDS]\n");
            return 1;
        }
    }
    if (delay < 0.1)
        delay = 0.1;
    if (batch) {
        for (long k = 0; count < 0 || k < count; k++) {
            if (k > 0) {
                usleep((useconds_t)(delay * 1e6));
                putchar('\n');
            }
            sample();
            have_prev = k > 0;
            frame(emit_stdout);
            fflush(stdout);
            have_prev = true;
        }
        return 0;
    }
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    timeout((int)(delay * 1000));
    for (long k = 0; count < 0 || k < count; k++) {
        sample();
        have_prev = k > 0;
        erase();
        row = 0;
        frame(emit_screen);
        mvprintw(LINES - 1, 0, "q: quit");
        refresh();
        have_prev = true;
        if (count >= 0 && k + 1 >= count)
            break;
        int ch = getch();
        if (ch == 'q' || ch == 'Q')
            break;
    }
    endwin();
    return 0;
}
