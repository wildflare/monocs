// glibc（aarch64）の主なヘッダーを、slimcc-a64 が解析できることを確かめる（-fsyntax-only）
#define _GNU_SOURCE 1
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <fenv.h>
#include <float.h>
#include <getopt.h>
#include <glob.h>
#include <inttypes.h>
#include <iso646.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <regex.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <stdalign.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/ucontext.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <threads.h>
#include <time.h>
#include <uchar.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

// AArch64 固有の定義が見えていること
_Static_assert(sizeof(mcontext_t) > 0, "mcontext_t");
_Static_assert(sizeof(jmp_buf) == sizeof(long) * 22 + sizeof(int) * 2 + sizeof(__sigset_t), "jmp_buf");
_Static_assert(WCHAR_MIN == 0, "wchar_t は符号なし");
_Static_assert(CHAR_MIN == 0, "char は符号なし");
_Static_assert(LDBL_MANT_DIG == 113, "long double は binary128");

int main(void)
{
    va_list ap;
    (void)ap;
    printf("%d\n", isalpha('a'));
    return 0;
}
