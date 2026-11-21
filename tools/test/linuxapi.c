// Linux のプログラムがよく使う関数（2026-11-14 に OS API の表に足したもの）を一通り呼んで、結果を表示する。
// 同じソースを Linux（gcc と glibc）と Monocs で動かして、出力を比べる（tools/test/linuxapi.sh）。
//   linuxapi -a -b x --long=y --flag file1 -- -file2
// 出力が環境で変わるもの（日時、乱数、パス）は表示しない
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <getopt.h>
#include <glob.h>
#include <locale.h>
#include <regex.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <dlfcn.h>
#include <pwd.h>
#include <wchar.h>
#include <wctype.h>

static volatile sig_atomic_t got;
static void on_sig(int sig) { got = sig; }
static void on_info(int sig, siginfo_t *info, void *ctx) { (void)ctx; got = 100 + (info->si_signo == sig); }   // 番号は Linux と picolibc で違うので出さない

static int cmp_r(const void *a, const void *b, void *arg)
{
    int dir = *(int *)arg;
    return dir * (*(const int *)a - *(const int *)b);
}

int main(int argc, char **argv)
{
    // ロケール: 始めは C、"" は環境変数（Monocs の既定は LANG=C.UTF-8。linuxapi.sh は Linux も LANG=C.UTF-8 で動かす）
    printf("locale start %s\n", setlocale(LC_ALL, NULL));
    printf("locale \"\" %s\n", setlocale(LC_ALL, ""));
    // ない言語のロケールは NULL（Linux で入れていないものと同じ。Pi 5 には pt_BR・ptb がない）
    printf("locale pt_BR %d ptb %d C %s POSIX %s\n", setlocale(LC_NUMERIC, "pt_BR") != NULL,
           setlocale(LC_NUMERIC, "ptb") != NULL, setlocale(LC_NUMERIC, "C"), setlocale(LC_NUMERIC, "POSIX"));
    setlocale(LC_ALL, "");

    // getopt_long（入れ替え: -- の前のファイルの名前は後ろに回る）
    static const struct option longs[] = {
        {"long", required_argument, NULL, 'L'}, {"flag", no_argument, NULL, 'F'}, {NULL, 0, NULL, 0},
    };
    printf("optind before: %d\n", optind);
    int c;
    while ((c = getopt_long(argc, argv, "ab:", longs, NULL)) != -1)
        printf("opt %c%s%s\n", c, optarg ? " arg=" : "", optarg ? optarg : "");
    for (int i = optind; i < argc; i++)
        printf("rest %s\n", argv[i]);

    // 環境変数
    setenv("MONOCS_TEST", "one", 1);
    setenv("MONOCS_TEST", "two", 0);
    printf("env %s\n", getenv("MONOCS_TEST"));
    char put[] = "MONOCS_PUT=yes";
    putenv(put);
    printf("putenv %s\n", getenv("MONOCS_PUT"));
    unsetenv("MONOCS_PUT");
    printf("unset %s\n", getenv("MONOCS_PUT") ? "still" : "gone");

    // 文字列
    char buf[] = "a,b,,c";
    char *p = buf, *t;
    while ((t = strsep(&p, ",")))
        printf("strsep [%s]\n", t);
    char d[16];
    printf("stpcpy %td\n", stpcpy(d, "hello") - d);
    printf("strlcpy %zu [%s]\n", strlcpy(d, "truncate me please", 8), d);
    printf("memmem %s\n", (char *)memmem("haystack", 8, "st", 2));
    printf("memrchr %s\n", (char *)memrchr("a/b/c", '/', 5));
    printf("strchrnul %d\n", *strchrnul("abc", 'z'));
    printf("strverscmp %d\n", strverscmp("file9", "file10") < 0);
    printf("index %s rindex %s ffs %d\n", index("a.b.c", '.'), rindex("a.b.c", '.'), ffs(0x50));
    char eb[64];
    printf("strerror_r %s\n", strerror_r(ENOENT, eb, sizeof eb));
    char *as;
    int n = asprintf(&as, "%d-%s", 42, "x");
    printf("asprintf %d %s\n", n, as);
    free(as);

    // open_memstream・fmemopen
    char *ms;
    size_t ml;
    FILE *f = open_memstream(&ms, &ml);
    fprintf(f, "mem %d", 7);
    fclose(f);
    printf("open_memstream %zu [%s]\n", ml, ms);
    free(ms);
    char src[] = "12 34";
    f = fmemopen(src, strlen(src), "r");
    int x, y;
    fscanf(f, "%d %d", &x, &y);
    fclose(f);
    printf("fmemopen %d\n", x + y);

    // qsort_r（GNU）、reallocarray、getsubopt
    int arr[] = {3, 1, 2};
    int dir = -1;
    qsort_r(arr, 3, sizeof(int), cmp_r, &dir);
    printf("qsort_r %d %d %d\n", arr[0], arr[1], arr[2]);
    int *ra = reallocarray(NULL, 4, sizeof(int));
    printf("reallocarray %s\n", ra ? "ok" : "fail");
    free(ra);
    char subopts[] = "rw,size=10";
    char *so = subopts, *val;
    char *const tokens[] = {"ro", "rw", "size", NULL};
    while (*so) {
        int k = getsubopt(&so, tokens, &val);
        printf("getsubopt %d %s\n", k, val ? val : "-");
    }

    // 時刻（固定の値）
    time_t tt = 86400 * 365;
    struct tm tm;
    gmtime_r(&tt, &tm);
    char ab[64];
    printf("asctime %s", asctime_r(&tm, ab));
    printf("timegm %lld\n", (long long)timegm(&tm));
    struct tm tm2 = {0};
    char *end = strptime("2026-11-14 12:34:56", "%Y-%m-%d %H:%M:%S", &tm2);
    printf("strptime %d %d %d %d:%d:%d rest=%d\n", tm2.tm_year, tm2.tm_mon, tm2.tm_mday, tm2.tm_hour, tm2.tm_min,
           tm2.tm_sec, end ? *end : -1);
    srandom(1);
    printf("random %s\n", random() >= 0 ? "ok" : "bad");

    // fnmatch・正規表現
    printf("fnmatch %d %d %d\n", fnmatch("*.c", "a.c", 0), fnmatch("*.c", "a.h", 0) != 0,
           fnmatch("*", ".x", FNM_PERIOD) != 0);
    regex_t re;
    regmatch_t m[3];
    if (regcomp(&re, "([a-z]+)=([0-9]+)", REG_EXTENDED) == 0) {
        if (regexec(&re, "  key=123 ", 3, m, 0) == 0)
            printf("regex %d-%d %d-%d %d-%d\n", (int)m[0].rm_so, (int)m[0].rm_eo, (int)m[1].rm_so,
                   (int)m[1].rm_eo, (int)m[2].rm_so, (int)m[2].rm_eo);
        printf("regex nomatch %d\n", regexec(&re, "none", 0, NULL, 0) == REG_NOMATCH);
        regfree(&re);
    }
    if (regcomp(&re, "a\\{2\\}b", 0) == 0) {    // BRE
        printf("regex bre %d %d\n", regexec(&re, "xaab", 0, NULL, 0), regexec(&re, "xab", 0, NULL, 0) != 0);
        regfree(&re);
    }
    char eb2[64];
    int rc = regcomp(&re, "(", REG_EXTENDED);
    regerror(rc, &re, eb2, sizeof eb2);
    printf("regerror %s\n", rc ? "error" : "none");

    // glob（パターンは / を含まない、どこで動かしても同じもの）
    glob_t g;
    printf("glob nomatch %d\n", glob("no-such-file-*", 0, NULL, &g) == GLOB_NOMATCH);
    if (glob("no-such-*", GLOB_NOCHECK, NULL, &g) == 0) {
        printf("glob nocheck %zu %s\n", g.gl_pathc, g.gl_pathv[0]);
        globfree(&g);
    }

    // ワイド文字
    const wchar_t *ws = L"日本語abc";
    printf("wcslen %zu wcswidth %d\n", wcslen(ws), wcswidth(ws, wcslen(ws)));
    wchar_t wb[32];
    wcscpy(wb, ws);
    wcscat(wb, L"!");
    printf("wcscmp %d wcschr %td wcsstr %td\n", wcscmp(wb, ws) > 0, wcschr(wb, L'a') - wb, wcsstr(wb, L"bc") - wb);
    printf("wcstol %ld\n", wcstol(L"  -123x", NULL, 10));
    printf("wcstod %.2f\n", wcstod(L"3.25", NULL));
    swprintf(wb, 32, L"%d:%ls", 5, L"かな");
    char mb[64];
    wcstombs(mb, wb, sizeof mb);
    printf("swprintf %s\n", mb);
    printf("isw %d %d %d %d\n", !!iswalpha(L'a'), !!iswdigit(L'5'), !!iswspace(L' '), !!iswupper(L'A'));
    printf("tow %lc %lc\n", (wint_t)towupper(L'a'), (wint_t)towlower(L'Z'));
    printf("isw-ja %d %d\n", !!iswalpha(L'あ'), !!iswprint(L'あ'));
    printf("tow-latin %x %x\n", (unsigned)towupper(0xE4), (unsigned)towlower(0xC4));
    int si = 0;
    wchar_t sw[16] = L"";
    int sn = swscanf(L"12 かなab", L"%d %ls", &si, sw);
    wcstombs(mb, sw, sizeof mb);
    printf("swscanf %d %d %s\n", sn, si, mb);
    printf("printf-ls [%ls] [%lc] [%5ls]\n", L"日本語", (wint_t)L'あ', L"ab");
    // シグナル（その場で届くもの）
    void (*prev)(int) = signal(SIGUSR1, on_sig);
    printf("signal prev dfl %d\n", prev == SIG_DFL);
    raise(SIGUSR1);
    printf("raise %d\n", got == SIGUSR1);
    got = 0;
    kill(getpid(), SIGUSR1);
    printf("kill %d kill0 %d\n", got == SIGUSR1, kill(getpid(), 0));
    struct sigaction sa = {0};
    sa.sa_sigaction = on_info;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);
    got = 0;
    raise(SIGUSR2);
    struct sigaction now;
    sigaction(SIGUSR2, NULL, &now);
    printf("siginfo %d resethand %d\n", (int)got, now.sa_handler == SIG_DFL);
    signal(SIGUSR2, on_sig);
    sigset_t set, pend;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    sigprocmask(SIG_BLOCK, &set, NULL);
    got = 0;
    raise(SIGUSR2);
    sigpending(&pend);
    printf("blocked %d pending %d\n", (int)got, sigismember(&pend, SIGUSR2));
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    printf("unblocked %d\n", got == SIGUSR2);
    signal(SIGUSR1, SIG_IGN);
    got = 0;
    raise(SIGUSR1);
    printf("ignored %d\n", (int)got);
    signal(SIGCHLD, SIG_DFL);
    raise(SIGCHLD);     // 既定で無視
    printf("sigchld default ignored\n");

    // popen
    FILE *pp = popen("echo hello; echo world", "r");
    char pl[64];
    while (pp && fgets(pl, sizeof pl, pp))
        printf("popen-r %s", pl);
    printf("pclose %d\n", pp ? pclose(pp) : -9);
    pp = popen("cat /no-such-file-monocs 2>/dev/null", "r");
    printf("pclose fail %d\n", pp ? WEXITSTATUS(pclose(pp)) : -9);
    fflush(stdout);
    pp = popen("wc -l", "w");
    if (pp) {
        fputs("a\nb\nc\n", pp);
        printf("pclose w %d\n", pclose(pp));
    }

    // ワイド文字の出力（1 つのストリームはバイト用とワイド用のどちらかなので、別のファイルに書いて読み直す）
    FILE *wf = fopen("linuxapi.tmp", "w");
    if (wf) {
        fputws(L"日本語", wf);
        fputwc(L'!', wf);
        fclose(wf);
        wf = fopen("linuxapi.tmp", "r");
        char rb[64] = "";
        size_t rn = fread(rb, 1, sizeof rb - 1, wf);
        rb[rn] = '\0';
        fclose(wf);
        printf("fputws %zu %s\n", rn, rb);
        wf = fopen("linuxapi.tmp", "r");
        wint_t w1 = fgetwc(wf), w2 = fgetwc(wf);
        fclose(wf);
        printf("fgetwc %x %x\n", (unsigned)w1, (unsigned)w2);
        remove("linuxapi.tmp");
    }

    // ファイルの終わりより先への lseek は大きさを変えず、write で 0 が入る。ftruncate・fsync・F_SETLK・mmap
    // （2026-11-20、SQLite で使うもの）
    int fd = open("linuxapi.db", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        struct stat st;
        char rb[16] = {1};
        off_t o = lseek(fd, 24, SEEK_SET);
        ssize_t rn = read(fd, rb, sizeof rb);
        fstat(fd, &st);
        printf("lseek past end %lld read %zd size %lld\n", (long long)o, rn, (long long)st.st_size);
        ssize_t wn = write(fd, "AB", 2);
        fstat(fd, &st);
        lseek(fd, 0, SEEK_SET);
        rn = read(fd, rb, sizeof rb);
        int zeros = 0;
        for (int i = 0; i < rn; i++)
            zeros += rb[i] == 0;
        printf("write after gap %zd size %lld read %zd zeros %d cur %lld\n", wn, (long long)st.st_size, rn, zeros,
               (long long)lseek(fd, 0, SEEK_CUR));
        lseek(fd, 5, SEEK_SET);
        int t1 = ftruncate(fd, 10000);
        fstat(fd, &st);
        long long sz1 = st.st_size, cur1 = lseek(fd, 0, SEEK_CUR);
        int t2 = ftruncate(fd, 3);
        fstat(fd, &st);
        printf("ftruncate %d size %lld cur %lld, %d size %lld cur %lld, fsync %d\n", t1, sz1, cur1, t2,
               (long long)st.st_size, (long long)lseek(fd, 0, SEEK_CUR), fsync(fd));
        struct flock lk = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1};
        int l1 = fcntl(fd, F_SETLK, &lk);
        lk.l_type = F_WRLCK;
        int l2 = fcntl(fd, F_GETLK, &lk);
        printf("lock %d getlk %d unlocked %d\n", l1, l2, lk.l_type == F_UNLCK);
        ftruncate(fd, 8192);
        char *m = mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m != MAP_FAILED) {
            int before = m[0] + m[1] + m[2] + m[4000];
            memcpy(m + 100, "mapped", 6);
            int r1 = munmap(m, 8192);
            char back[7] = "";
            lseek(fd, 100, SEEK_SET);
            read(fd, back, 6);
            printf("mmap %d munmap %d back %s\n", before, r1, back);
        }
        char *a = mmap(nullptr, 100, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        printf("anon %d aligned %d\n", a != MAP_FAILED && a[50] == 0, ((unsigned long)a & 4095) == 0);
        munmap(a, 100);
        close(fd);
        struct stat s1, s2, s3;
        fd = open("linuxapi2.db", O_RDWR | O_CREAT, 0644);
        close(fd);
        stat("linuxapi.db", &s1);
        stat("linuxapi2.db", &s2);
        stat("./linuxapi.db", &s3);
        printf("ino differ %d same %d\n", s1.st_ino != s2.st_ino, s1.st_ino == s3.st_ino && s1.st_dev == s3.st_dev);
        remove("linuxapi.db");
        remove("linuxapi2.db");
    }
    // 読み出し専用で先に開いた fd・FILE からも、ほかで書いて閉じたものが見える
    {
        FILE *w = fopen("linuxapi.vis", "w");
        FILE *r = fopen("linuxapi.vis", "r");
        int fd2 = open("linuxapi.vis", O_RDONLY);
        char b1[16] = "", b2[16] = "";
        size_t n0 = fread(b1, 1, sizeof b1, r);
        fputs("hello", w);
        fclose(w);
        fseek(r, 0, SEEK_SET);
        size_t n1 = fread(b1, 1, sizeof b1 - 1, r);
        struct stat st2;
        fstat(fd2, &st2);
        ssize_t n2 = read(fd2, b2, sizeof b2 - 1);
        b1[n1] = '\0';
        b2[n2 > 0 ? n2 : 0] = '\0';
        FILE *r2 = fopen("linuxapi.vis", "r");
        char b3[16] = "";
        size_t n3 = fread(b3, 1, sizeof b3 - 1, r2);
        b3[n3] = '\0';
        printf("visible %zu %zu %s %lld %zd %s %s\n", n0, n1, b1, (long long)st2.st_size, n2, b2, b3);
        fclose(r);
        fclose(r2);
        close(fd2);
        // O_APPEND: lseek しても write は終わりへ。書いたそばから、ほかの fd で読める
        int wa = open("linuxapi.vis", O_WRONLY | O_APPEND);
        int rd = open("linuxapi.vis", O_RDONLY);
        lseek(wa, 0, SEEK_SET);
        write(wa, "XY", 2);
        char b4[16] = "";
        ssize_t n4 = read(rd, b4, sizeof b4 - 1);
        b4[n4 > 0 ? n4 : 0] = '\0';
        printf("append %zd %s %lld\n", n4, b4, (long long)lseek(wa, 0, SEEK_CUR));
        close(wa);
        close(rd);
        remove("linuxapi.vis");
    }
    // pipe（同じプログラムの中。2026-11-21）
    {
        int pf[2];
        int r = pipe(pf);
        char b[16] = {0};
        ssize_t w = write(pf[1], "hello", 5);
        struct pollfd pp[2] = {{pf[0], POLLIN, 0}, {pf[1], POLLOUT, 0}};
        int pr = poll(pp, 2, 0);
        int avail = -1;
        ioctl(pf[0], FIONREAD, &avail);
        ssize_t rd = read(pf[0], b, sizeof b);
        fcntl(pf[0], F_SETFL, fcntl(pf[0], F_GETFL) | O_NONBLOCK);
        ssize_t again = read(pf[0], b + 8, 1);
        int e_again = errno == EAGAIN;
        struct stat pst;
        fstat(pf[0], &pst);
        int acc = fcntl(pf[1], F_GETFL) & O_ACCMODE;
        close(pf[1]);
        ssize_t eof = read(pf[0], b + 8, 1);
        pp[0].revents = 0;
        poll(pp, 1, 0);
        printf("pipe %d w %zd poll %d %x %x avail %d read %zd '%s' again %zd %d fifo %d acc %d eof %zd hup %d\n", r, w,
               pr, pp[0].revents & POLLIN, pp[1].revents, avail, rd, b, again, e_again, S_ISFIFO(pst.st_mode),
               acc == O_WRONLY, eof, (pp[0].revents & (POLLIN | POLLHUP)) != 0);
        close(pf[0]);
        signal(SIGPIPE, SIG_IGN);
        pipe(pf);
        close(pf[0]);
        errno = 0;
        ssize_t ep = write(pf[1], "x", 1);
        printf("pipe epipe %zd %d\n", ep, errno == EPIPE);
        close(pf[1]);
        signal(SIGPIPE, SIG_DFL);
    }
    // struct tm の tm_gmtoff・tm_zone（glibc と同じ。2026-11-20）
    {
        char *old_tz = getenv("TZ") ? strdup(getenv("TZ")) : NULL;
        setenv("TZ", "JST-9", 1);
        tzset();
        time_t t0 = 1700000000;
        struct tm lt, gt;
        localtime_r(&t0, &lt);
        gmtime_r(&t0, &gt);
        struct tm *lp = localtime(&t0);
        struct tm mk = {.tm_year = 126, .tm_mon = 10, .tm_mday = 20, .tm_hour = 12, .tm_isdst = -1};
        time_t mt = mktime(&mk);
        printf("tm_gmtoff local %ld %s gm %ld %s localtime %ld %s mktime %lld %ld %s\n", lt.tm_gmtoff, lt.tm_zone,
               gt.tm_gmtoff, gt.tm_zone, lp->tm_gmtoff, lp->tm_zone, (long long)mt, mk.tm_gmtoff, mk.tm_zone);
        if (old_tz)
            setenv("TZ", old_tz, 1);
        else
            unsetenv("TZ");
        tzset();
        free(old_tz);
    }
    struct rusage ru;
    struct rlimit rl;
    printf("getrusage %d getrlimit %d nofile %d\n", getrusage(RUSAGE_SELF, &ru), getrlimit(RLIMIT_NOFILE, &rl),
           rl.rlim_cur >= 64);
    void *dl = dlopen("libnothere.so", RTLD_NOW);
    printf("dlopen %d error %d\n", dl != nullptr, dlerror() != nullptr);
    // GCC の __atomic_*・__sync_*・数学の組み込み（slimcc は 2026-11-20 から）
    long av = 5, exp = 5, out = 0;
    unsigned char flag = 0;
    long a1 = __atomic_fetch_add(&av, 3, __ATOMIC_SEQ_CST), a2 = __atomic_add_fetch(&av, 2, __ATOMIC_RELAXED);
    bool c1 = __atomic_compare_exchange_n(&av, &exp, 100, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    long e1 = exp, x1 = __atomic_exchange_n(&av, 7, __ATOMIC_ACQ_REL);
    __atomic_store_n(&av, 9, __ATOMIC_RELEASE);
    __atomic_load(&av, &out, __ATOMIC_ACQUIRE);
    bool t1 = __atomic_test_and_set(&flag, __ATOMIC_SEQ_CST), t2 = __atomic_test_and_set(&flag, __ATOMIC_SEQ_CST);
    __atomic_clear(&flag, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    printf("atomic %ld %ld %d %ld %ld %ld %ld %d %d %d %d\n", a1, a2, c1, e1, x1, out, __atomic_load_n(&av, __ATOMIC_RELAXED),
           t1, t2, flag, __atomic_always_lock_free(sizeof av, 0));
    int sv = 10;
    int s1 = __sync_fetch_and_sub(&sv, 4), s2 = __sync_or_and_fetch(&sv, 1);
    int s3 = __sync_val_compare_and_swap(&sv, 7, 42), s4 = __sync_bool_compare_and_swap(&sv, 1, 2);
    int s5 = __sync_lock_test_and_set(&sv, 3);
    __sync_synchronize();
    __sync_lock_release(&sv);
    printf("sync %d %d %d %d %d %d\n", s1, s2, s3, s4, s5, sv);
    volatile double dv = -2.5;
    printf("builtin math %g %g %g %g %g %g %g\n", __builtin_ceil(dv), __builtin_floor(dv), __builtin_trunc(dv),
           __builtin_round(dv), __builtin_fabs(dv), __builtin_sqrt(6.25), (double)__builtin_floorf(1.5f));
    struct passwd *pw = getpwuid(0);
    printf("getpwuid %s %s\n", pw ? pw->pw_name : "-", pw ? pw->pw_dir : "-");
    return 0;
}
