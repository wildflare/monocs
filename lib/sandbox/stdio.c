// サンドボックスの中の stdio（書く先は SB_SYS_WRITE だけ。読む・ファイルはまだない）。printf の書式は、よく使うもの:
// %d %i %u %x %X %o %c %s %p %f %e %g %% と、- 0 + 空白 # の印、幅・精度（* も）、h hh l ll z j t
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sandbox_sys.h"

static struct __file out_file, err_file;
FILE *const stdout = &out_file;
FILE *const stderr = &err_file;
FILE *const stdin = nullptr;

#define BUF 1024
static char out_buf[BUF];
static size_t out_len;

static void out_flush(void)
{
    if (out_len)
        sb_sys(SB_SYS_WRITE, 1, (long)out_buf, (long)out_len);
    out_len = 0;
}

static void put_to(FILE *f, char c)
{
    if (f == stderr) {
        out_flush();
        sb_sys(SB_SYS_WRITE, 2, (long)&c, 1);
        return;
    }
    out_buf[out_len++] = c;
    if (c == '\n' || out_len == BUF)
        out_flush();
}

int fflush(FILE *f)
{
    (void)f;
    out_flush();
    return 0;
}

int fputc(int c, FILE *f)
{
    put_to(f, (char)c);
    return (unsigned char)c;
}

int (putc)(int c, FILE *f)     // かっこ: stdio.h のマクロにされないように
{
    return fputc(c, f);
}

int (putchar)(int c)
{
    return fputc(c, stdout);
}

int fputs(const char *s, FILE *f)
{
    while (*s)
        put_to(f, *s++);
    return 0;
}

int puts(const char *s)
{
    fputs(s, stdout);
    put_to(stdout, '\n');
    return 0;
}

size_t fwrite(const void *p, size_t size, size_t n, FILE *f)
{
    const char *c = p;
    for (size_t i = 0; i < size * n; i++)
        put_to(f, c[i]);
    return n;
}

// ---- 書式 ----
struct sink {
    FILE *f;            // ファイルへ（NULL なら文字列へ）
    char *buf;
    size_t cap, len;    // len は書いたはずの長さ（入りきらなくても数える）
};

static void emit(struct sink *s, char c)
{
    if (s->f)
        put_to(s->f, c);
    else if (s->len + 1 < s->cap)
        s->buf[s->len] = c;
    s->len++;
}

static void emit_n(struct sink *s, const char *p, size_t n, int width, bool left, char pad)
{
    int w = width > (int)n ? width - (int)n : 0;
    if (!left)
        while (w-- > 0)
            emit(s, pad);
    for (size_t i = 0; i < n; i++)
        emit(s, p[i]);
    if (left)
        while (w-- > 0)
            emit(s, ' ');
}

// 整数を文字にする（逆順に作って返す）
static int utoa(char *out, unsigned long long v, unsigned base, bool upper)
{
    const char *d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[32];
    int n = 0;
    do
        tmp[n++] = d[v % base];
    while ((v /= base));
    for (int i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

// 浮動小数点（%f %e %g。正確な丸めはしない簡単なもの: 17 桁まで）
static int ftoa(char *out, double v, int prec, char conv)
{
    int n = 0;
    if (v != v)
        return (memcpy(out, "nan", 3), 3);
    if (v < 0)
        v = -v;
    if (v > 1.8e308)
        return (memcpy(out, "inf", 3), 3);
    int exp10 = 0;
    bool use_e = conv == 'e';
    if (conv == 'g') {
        double t = v;
        int e = 0;
        if (t != 0) {
            while (t >= 10) t /= 10, e++;
            while (t < 1) t *= 10, e--;
        }
        if (prec == 0)
            prec = 1;
        use_e = e < -4 || e >= prec;
        prec = use_e ? prec - 1 : prec - 1 - e;
        if (prec < 0)
            prec = 0;
    }
    if (use_e && v != 0) {
        while (v >= 10) v /= 10, exp10++;
        while (v < 1) v *= 10, exp10--;
    }
    double r = 0.5;
    for (int i = 0; i < prec; i++)
        r /= 10;
    v += r;
    if (use_e && v >= 10)
        v /= 10, exp10++;
    unsigned long long ip = (unsigned long long)v;
    double fp = v - (double)ip;
    n += utoa(out + n, ip, 10, false);
    if (prec > 0) {
        out[n++] = '.';
        for (int i = 0; i < prec && i < 40; i++) {
            fp *= 10;
            int d = (int)fp;
            out[n++] = (char)('0' + d);
            fp -= d;
        }
    }
    if (conv == 'g' && prec > 0) {      // %g は終わりの 0 と点を取る
        while (out[n - 1] == '0')
            n--;
        if (out[n - 1] == '.')
            n--;
    }
    if (use_e) {
        out[n++] = 'e';
        out[n++] = exp10 < 0 ? '-' : '+';
        unsigned e = (unsigned)(exp10 < 0 ? -exp10 : exp10);
        if (e < 10)
            out[n++] = '0';
        n += utoa(out + n, e, 10, false);
    }
    return n;
}

static void format(struct sink *s, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit(s, *fmt);
            continue;
        }
        fmt++;
        bool left = false, plus = false, space = false, alt = false;
        char pad = ' ';
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') pad = '0';
            else if (*fmt == '+') plus = true;
            else if (*fmt == ' ') space = true;
            else if (*fmt == '#') alt = true;
            else break;
        }
        int width = 0, prec = -1;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) left = true, width = -width;
            fmt++;
        } else
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + (*fmt++ - '0');
        }
        int lng = 0;    // 0: int、1: long、2: long long、-1: short、-2: char
        for (;; fmt++) {
            if (*fmt == 'l') lng++;
            else if (*fmt == 'h') lng--;
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') lng = 2;
            else break;
        }
        char c = *fmt, num[64];
        if (!c)
            break;
        if (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X' || c == 'o' || c == 'p') {
            unsigned long long v;
            bool neg = false;
            if (c == 'p') {
                v = (uintptr_t)va_arg(ap, void *);
                alt = true;
                c = 'x';
            } else if (c == 'd' || c == 'i') {
                long long x = lng >= 2 ? va_arg(ap, long long) : lng == 1 ? va_arg(ap, long) : va_arg(ap, int);
                if (lng == -1) x = (short)x;
                if (lng <= -2) x = (signed char)x;
                neg = x < 0;
                v = neg ? 0ULL - (unsigned long long)x : (unsigned long long)x;
            } else {
                v = lng >= 2 ? va_arg(ap, unsigned long long) : lng == 1 ? va_arg(ap, unsigned long)
                                                                         : va_arg(ap, unsigned);
                if (lng == -1) v = (unsigned short)v;
                if (lng <= -2) v = (unsigned char)v;
            }
            int n = 0;
            char sign = neg ? '-' : plus ? '+' : space ? ' ' : 0;
            if (sign && (c == 'd' || c == 'i'))
                num[n++] = sign;
            if (alt && (c == 'x' || c == 'X') && v)
                num[n++] = '0', num[n++] = c;
            int start = n;
            char digits[32];
            int nd = (prec == 0 && v == 0) ? 0 : utoa(digits, v, c == 'o' ? 8 : (c == 'u' || c == 'd' || c == 'i') ? 10 : 16, c == 'X');
            for (int z = nd; z < prec; z++)
                num[n++] = '0';
            memcpy(num + n, digits, (size_t)nd);
            n += nd;
            if (pad == '0' && !left && prec < 0 && width > n) {   // 0 埋めは符号の後に
                emit_n(s, num, (size_t)start, 0, false, ' ');
                emit_n(s, num + start, (size_t)(n - start), width - start, false, '0');
            } else
                emit_n(s, num, (size_t)n, width, left, ' ');
        } else if (c == 'f' || c == 'F' || c == 'e' || c == 'g') {
            double v = va_arg(ap, double);
            int n = 0;
            if (v < 0 || (v == 0 && 1 / v < 0)) num[n++] = '-';
            else if (plus) num[n++] = '+';
            else if (space) num[n++] = ' ';
            n += ftoa(num + n, v, prec < 0 ? 6 : prec, c == 'F' ? 'f' : c);
            emit_n(s, num, (size_t)n, width, left, pad == '0' && !left ? '0' : ' ');
        } else if (c == 'c') {
            char ch = (char)va_arg(ap, int);
            emit_n(s, &ch, 1, width, left, ' ');
        } else if (c == 's') {
            const char *str = va_arg(ap, const char *);
            if (!str)
                str = "(null)";
            size_t n = strlen(str);
            if (prec >= 0 && (size_t)prec < n)
                n = (size_t)prec;
            emit_n(s, str, n, width, left, ' ');
        } else if (c == '%') {
            emit(s, '%');
        } else {
            emit(s, '%');
            emit(s, c);
        }
    }
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    struct sink s = { f ? f : stdout, nullptr, 0, 0 };
    format(&s, fmt, ap);
    return (int)s.len;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int vprintf(const char *fmt, va_list ap)
{
    return vfprintf(stdout, fmt, ap);
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    struct sink s = { nullptr, buf, cap, 0 };
    format(&s, fmt, ap);
    if (cap)
        buf[s.len < cap ? s.len : cap - 1] = 0;
    return (int)s.len;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return r;
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
    return vsnprintf(buf, (size_t)-1 / 2, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}
