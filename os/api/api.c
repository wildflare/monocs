// OS API の関数表。一覧は api.def

// POSIX・BSD の関数（strdup など）の宣言も見えるようにする
#define _DEFAULT_SOURCE 1

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utime.h>
#include <sys/time.h>
#include <time.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wchar.h>
#include <libgen.h>
#include <sys/stat.h>
// GNU の拡張（string.h は _GNU_SOURCE のときだけ宣言する）と、posix.c の lstat
char *strcasestr(const char *haystack, const char *needle);
int lstat(const char *restrict path, struct stat *restrict st);
size_t __locale_mb_cur_max(void);
// GNU 版の strerror_r・qsort_r（カーネルの api.c は _GNU_SOURCE でないので、ヘッダーはこの名前を POSIX 版・BSD 版に
// 読み替える。アプリが qsort_r・strerror_r の名前で探すのは、_GNU_SOURCE でコンパイルしたもの）
// アプリの setlocale（os/libc/posix.c。posix.h は sys/types.h を読み、lwIP の BYTE_ORDER とぶつかるので、宣言だけ）
char *app_setlocale(int category, const char *name);
int app_fflush(FILE *f);    // fflush(NULL) は標準出力と標準エラー（os/libc/posix.c）
// tm_gmtoff・tm_zone も埋める localtime などの版（os/libc/posix.c。表には番地だけ要るので、形は問わない）
extern char app_localtime[], app_localtime_r[], app_gmtime[], app_gmtime_r[], app_mktime[], app_timegm[];
extern char gnu_strerror_r[] __asm__("strerror_r");
extern char gnu_qsort_r[] __asm__("qsort_r");
#include "../kernel/exception.h"
#include "../kernel/app.h"
#include "../libc/env.h"
#include "../kernel/clock.h"
#include "../net/udp_api.h"
#include "../net/dns_api.h"
#include "../kernel/sha256.h"
#include "../drivers/wifi_auto.h"
#include "api.h"
#include "include/curses.h"

// picolibc の wchar.h は XSI（_XOPEN_SOURCE）のときだけ宣言する。カーネルは -std=c23 なので自分で書く
int wcwidth(wchar_t c);
#include "../drivers/led.h"
#include "../drivers/timer.h"
#include "../drivers/uart.h"
#include "../slimcc/cmd.h"
#include "../kernel/lock.h"
#include "../kernel/smp.h"
#include "../kernel/term.h"
#include "../kernel/contap.h"
#include "../kernel/rexec.h"
#include "include/monocs_sysstat.h"
void sysstat_get(MonocsSysStat *st);
#include "../kernel/klog.h"
#include "include/pthread.h"
#include "include/omp.h"
#include "../libc/omp_os.h"
#include "../net/tcp_api.h"
#include "../fs/vfs.h"
#include "../shell/shell.h"

// ヘッダーにない関数は、アドレスだけ使うので型を問わない宣言にする
#define API(name)
#define API_EXT(name) extern char name[];
#define API_AS(name, impl)
// TLS の変数のアドレスはコアごとに違い、定数にならないので、求める関数を作る
#define API_TLS(name) static const void *tls_addr_##name(void) { return &(name); }
#include "api.def"
#undef API
#undef API_EXT
#undef API_AS
#undef API_TLS

// errno は picolibc の errno.h で _Thread_local の変数として宣言される
const struct os_api_entry os_api_table[] = {
#define API(name) { #name, (const void *)&(name), false },
#define API_EXT(name) { #name, (const void *)(name), false },
#define API_AS(name, impl) { #name, (const void *)&(impl), false },
#define API_TLS(name) { #name, (const void *)tls_addr_##name, true },
#include "api.def"
#undef API
#undef API_EXT
#undef API_AS
#undef API_TLS
};

const size_t os_api_count = sizeof(os_api_table) / sizeof(os_api_table[0]);
