// シェルの 1 行の編集（readline・linenoise と同じ考え方。curses は使わず、ふつうの画面のまま行だけを書き直す）
//
//   ← →、Home・End、Ctrl-A・Ctrl-E・Ctrl-B・Ctrl-F: カーソルを動かす。Alt-B・Alt-F: 単語ずつ
//   ↑ ↓、Ctrl-P・Ctrl-N: 履歴（/root/.shell_history に残す。電源を切っても残る）
//   Backspace、Delete、Ctrl-D（行が空なら終わり）: 1 文字消す。Ctrl-W・Alt-Backspace: 前の単語、Alt-D: 次の単語
//   Ctrl-U: カーソルより前を消す、Ctrl-K: カーソルより後を消す、Ctrl-L: 画面を消して描き直す
//   Tab: コマンド名（行の最初の語）・ファイル名を補う。候補がいくつもあれば、共通の部分まで補い、なければ一覧を出す
//   Ctrl-C: 行を捨てる
//
// 文字を行の終わりに足すだけなら、打った文字をそのまま返す（今までと同じ。nos.py などの自動の操作は変わらない）。
// 行を書き直すときは、端末の桁数を使う（telnet なら知らされた大きさ、ほかは初めて書き直すときに
// 問い合わせる。os/kernel/term.c）。端末の幅を越えて折り返した行は、linenoise の複数行の方式と同じ計算で
// 書き直す（出典: linenoise の refreshMultiLine。行の数とカーソルの行を表示幅から求める）。

// カーネルは -std=c23（ISO）でビルドするので、POSIX の宣言（strdup、strncasecmp など）を見えるようにする
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <wchar.h>
#include "../kernel/clock.h"
#include "lineedit.h"
#include "../drivers/timer.h"
#include "../drivers/uart.h"
#include "../fs/vfs.h"
#include "../kernel/contap.h"
#include "../kernel/svc.h"
#include "../kernel/term.h"
#include "../kernel/klog_file.h"

int wcwidth(wchar_t c);

#define HISTORY_FILE    "/root/.shell_history"
#define HISTORY_MAX     200
#define ESC_WAIT_US     50000   // ESC の後、続きのバイトを待つ時間

enum {
    K_UP = 0x100, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DELETE,
    K_WORD_LEFT, K_WORD_RIGHT, K_WORD_DELETE, K_WORD_BACKSPACE, K_NONE,
};

typedef struct {
    const char *prompt;
    char *buf;
    int size, len, pos;     // バイト数。pos は文字の区切りにある
    int plen;               // プロンプトの表示幅
    int cols;               // 端末の桁数（0 はまだ知らない）
    int shown_cur;          // 表示しているカーソルの位置（プロンプトの先頭からの表示幅）
    int shown_total;        // 表示している行全体の表示幅
    int maxrows;            // 今までに使った行の数（0 は、まだ書き直していない）
    bool echoed;            // 最後の表示が、打った文字をそのまま返したもの（書き直しではない）
} Edit;

// ---- 出力 ----

static void out(const char *s)
{
    uart_puts(s);
}

static void outn(const char *s, int n)
{
    for (int i = 0; i < n; i++)
        uart_putc(s[i]);
}

static void outf(const char *fmt, int v)
{
    char b[32];
    snprintf(b, sizeof(b), fmt, v);
    out(b);
}

// ---- UTF-8 と表示幅 ----

static int width_of(const char *s, int n);

static int char_len(unsigned char c)
{
    return c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
}

static int width_of(const char *s, int n)
{
    int w = 0;
    for (int i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        int l = char_len(c);
        if (i + l > n)
            l = n - i;
        unsigned cp = l == 1 ? c : (unsigned)(c & (0x3F >> (l - 1)));
        for (int k = 1; k < l; k++)
            cp = cp << 6 | ((unsigned char)s[i + k] & 0x3F);
        int cw = wcwidth((wchar_t)cp);
        w += cw < 0 ? 1 : cw;
        i += l;
    }
    return w;
}

static int prev_char(const Edit *e, int p)
{
    if (p <= 0)
        return 0;
    do
        p--;
    while (p > 0 && ((unsigned char)e->buf[p] & 0xC0) == 0x80);
    return p;
}

static int next_char(const Edit *e, int p)
{
    if (p >= e->len)
        return e->len;
    return p + char_len((unsigned char)e->buf[p]) <= e->len ? p + char_len((unsigned char)e->buf[p]) : e->len;
}

// ---- 入力 ----

static void (*idle_fn)(void);

void line_edit_set_idle(void (*fn)(void))
{
    idle_fn = fn;
}

static int read_byte_wait(void)
{
    // 入力を待つ間に、遅らせている SD カードへの書き込みと、サービスのコアの見張りを進める
    while (!uart_has_input()) {
        klog_file_sync(false);  // 記録を /var/log/dmesg に書き足す（5 秒に 1 回まで）
        clock_save(false);      // 今の時刻を覚える（60 秒に 1 回まで。os/kernel/clock.c）
        vfs_sync(false);
        svc_check();
        if (idle_fn)
            idle_fn();
        uart_wait_input();  // 次の割り込み（キー入力か、タイマー）まで眠る
    }
    return (unsigned char)uart_getc();
}

// 続きのバイトを少しだけ待つ。来なければ -1
static int read_byte_soon(void)
{
    uint64_t end = timer_now_us() + ESC_WAIT_US;
    while (!uart_has_input()) {
        if (timer_now_us() >= end)
            return -1;
        uart_wait_input();
    }
    return (unsigned char)uart_getc();
}

// キーを 1 つ読む（エスケープシーケンスは K_* に。知らないものは K_NONE）
static int read_key(void)
{
    int c = read_byte_wait();
    if (c != 0x1B)
        return c;
    int c2 = read_byte_soon();
    if (c2 < 0)
        return K_NONE;  // ESC だけ
    if (c2 == '[' || c2 == 'O') {
        // CSI（ESC [ 数字 ; 数字 最後の文字）と SS3（ESC O 文字）
        int num = 0, c3;
        while ((c3 = read_byte_soon()) >= 0 && !(c3 >= 0x40 && c3 <= 0x7E))
            if (c3 >= '0' && c3 <= '9' && num < 100)
                num = num * 10 + (c3 - '0');
            else if (c3 == ';')
                num = num ? num : 1;    // ESC [ 1 ; 5 C などは 1 の方を使う
        switch (c3) {
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        case 'H': return K_HOME;
        case 'F': return K_END;
        case '~':
            switch (num) {
            case 1: case 7: return K_HOME;
            case 4: case 8: return K_END;
            case 3: return K_DELETE;
            }
            return K_NONE;
        }
        return K_NONE;
    }
    switch (c2) {   // Alt と文字
    case 'b': return K_WORD_LEFT;
    case 'f': return K_WORD_RIGHT;
    case 'd': return K_WORD_DELETE;
    case 0x7F: case 0x08: return K_WORD_BACKSPACE;
    }
    return K_NONE;
}

// ---- 書き直し ----

static void ensure_cols(Edit *e)
{
    int rows, cols;
    if (contap_size(&rows, &cols) && cols > 0) {    // telnet: 知らされた大きさ（変わることがある）
        e->cols = cols;
        return;
    }
    if (e->cols)
        return;
    term_size(&rows, &cols);
    e->cols = cols > 0 ? cols : 80;
}

static void refresh(Edit *e)
{
    ensure_cols(e);
    int cols = e->cols;
    int plen = e->plen;
    int len = width_of(e->buf, e->len);
    int pos = width_of(e->buf, e->pos);
    if (!e->maxrows) {
        // 初めての書き直し: 今までは打った文字をそのまま返していた。その分の行の数から始める
        e->maxrows = (e->shown_total + cols - 1) / cols;
        if (e->maxrows < 1)
            e->maxrows = 1;
    }
    // 表示しているカーソルの行（1 から）。打った文字で右端までちょうど埋まった直後は、端末のカーソルは
    // まだその行にある（折り返しは次の文字まで保留される）。書き直しでは、その場合に次の行へ出している
    int rpos = (e->shown_cur + cols) / cols;
    if (e->echoed && e->shown_cur > 0 && e->shown_cur % cols == 0)
        rpos--;
    e->echoed = false;
    int rows = (plen + len + cols - 1) / cols;
    if (rows < 1)
        rows = 1;
    int old_rows = e->maxrows;
    if (rows > e->maxrows)
        e->maxrows = rows;

    // いちばん下の行へ行き、下から順に消す
    if (old_rows - rpos > 0)
        outf("\x1b[%dB", old_rows - rpos);
    for (int j = 0; j < old_rows - 1; j++)
        out("\r\x1b[K\x1b[1A");
    out("\r\x1b[K");
    out(e->prompt);
    outn(e->buf, e->len);
    int total = plen + len;
    // 行の終わりにカーソルがあり、ちょうど右端なら、次の行の頭へ出しておく
    if (e->pos == e->len && pos > 0 && total % cols == 0) {
        out("\n");
        rows++;
        if (rows > e->maxrows)
            e->maxrows = rows;
    }
    int cur = plen + pos;
    int rpos2 = (cur + cols) / cols;
    if (rows - rpos2 > 0)
        outf("\x1b[%dA", rows - rpos2);
    out("\r");
    if (cur % cols)
        outf("\x1b[%dC", cur % cols);
    e->shown_cur = cur;
    e->shown_total = total;
}

// 行を初めから表示する（一覧を出した後、Ctrl-L の後）
static void redraw_fresh(Edit *e)
{
    out(e->prompt);
    outn(e->buf, e->len);
    e->shown_cur = e->shown_total = e->plen + width_of(e->buf, e->len);
    e->maxrows = 0;
    e->echoed = true;
    if (e->pos != e->len)
        refresh(e);
}

// ---- 編集 ----

static void insert(Edit *e, const char *s, int n)
{
    if (e->len + n >= e->size)
        return;
    memmove(e->buf + e->pos + n, e->buf + e->pos, (size_t)(e->len - e->pos));
    memcpy(e->buf + e->pos, s, (size_t)n);
    e->len += n;
    e->pos += n;
    e->buf[e->len] = '\0';
    if (e->pos == e->len && !e->maxrows) {
        // 行の終わりに足すだけ: そのまま返す（端末が折り返す）
        outn(s, n);
        e->shown_cur = e->shown_total = e->plen + width_of(e->buf, e->len);
        e->echoed = true;
    } else {
        refresh(e);
    }
}

static void delete_range(Edit *e, int from, int to)
{
    if (from >= to)
        return;
    memmove(e->buf + from, e->buf + to, (size_t)(e->len - to));
    e->len -= to - from;
    e->buf[e->len] = '\0';
    if (e->pos > to)
        e->pos -= to - from;
    else if (e->pos > from)
        e->pos = from;
    refresh(e);
}

static bool is_space(char c)
{
    return c == ' ' || c == '/';
}

static int word_left(const Edit *e, int p)
{
    while (p > 0 && is_space(e->buf[p - 1]))
        p--;
    while (p > 0 && !is_space(e->buf[p - 1]))
        p--;
    return p;
}

static int word_right(const Edit *e, int p)
{
    while (p < e->len && is_space(e->buf[p]))
        p++;
    while (p < e->len && !is_space(e->buf[p]))
        p++;
    return p;
}

static void set_line(Edit *e, const char *s)
{
    int n = (int)strlen(s);
    if (n >= e->size)
        n = e->size - 1;
    memcpy(e->buf, s, (size_t)n);
    e->buf[n] = '\0';
    e->len = e->pos = n;
    refresh(e);
}

// ---- 履歴 ----

static char *history[HISTORY_MAX];
static int nhistory;
static bool history_loaded;

static void history_push(const char *line)
{
    if (nhistory && !strcmp(history[nhistory - 1], line))
        return;     // 続けて同じものは 1 つ
    if (nhistory == HISTORY_MAX) {
        free(history[0]);
        memmove(history, history + 1, sizeof(char *) * (HISTORY_MAX - 1));
        nhistory--;
    }
    size_t n = strlen(line) + 1;
    char *s = malloc(n);
    if (!s)
        return;
    memcpy(s, line, n);
    history[nhistory++] = s;
}

static void history_load(void)
{
    if (history_loaded)
        return;
    history_loaded = true;
    FILE *f = fopen(HISTORY_FILE, "r");
    if (!f)
        return;
    char line[LINE_EDIT_MAX];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0])
            history_push(line);
    }
    fclose(f);
}

static void history_save(void)
{
    FILE *f = fopen(HISTORY_FILE ".tmp", "w");
    if (!f)
        return;
    for (int i = 0; i < nhistory; i++)
        fprintf(f, "%s\n", history[i]);
    if (fclose(f) == 0)
        rename(HISTORY_FILE ".tmp", HISTORY_FILE);
}

int line_edit_history_count(void)
{
    history_load();
    return nhistory;
}

const char *line_edit_history_get(int i)
{
    return i >= 0 && i < nhistory ? history[i] : nullptr;
}

void line_edit_history_clear(void)
{
    history_load();
    for (int i = 0; i < nhistory; i++)
        free(history[i]);
    nhistory = 0;
    history_save();
}

void line_edit_add_history(const char *line)
{
    if (!line[0] || line[0] == ' ')
        return;     // 空、または空白で始まる行は残さない（bash の ignorespace と同じ）
    history_load();
    history_push(line);
    history_save();
}

// ---- 補完 ----

#define MAX_CANDS 256

typedef struct {
    char *names[MAX_CANDS];
    bool dir[MAX_CANDS];
    int n;
} Cands;

static void cands_add(Cands *c, const char *name, bool dir)
{
    if (c->n == MAX_CANDS)
        return;
    for (int i = 0; i < c->n; i++)
        if (!strcmp(c->names[i], name))
            return;
    size_t n = strlen(name) + 1;
    char *s = malloc(n);
    if (!s)
        return;
    memcpy(s, name, n);
    c->dir[c->n] = dir;
    c->names[c->n++] = s;
}

static void cands_free(Cands *c)
{
    for (int i = 0; i < c->n; i++)
        free(c->names[i]);
    c->n = 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static void list_cands(Edit *e, Cands *c)
{
    ensure_cols(e);
    // 名前の順に、桁をそろえて並べる
    qsort(c->names, (size_t)c->n, sizeof(char *), cmp_str);
    int w = 0;
    for (int i = 0; i < c->n; i++) {
        int l = width_of(c->names[i], (int)strlen(c->names[i])) + 2;
        if (l > w)
            w = l;
    }
    int per = e->cols / w > 0 ? e->cols / w : 1;
    // カーソルを行の終わりの後へ
    e->pos = e->len;
    refresh(e);
    out("\n");
    for (int i = 0; i < c->n; i++) {
        out(c->names[i]);
        int l = width_of(c->names[i], (int)strlen(c->names[i]));
        if ((i + 1) % per == 0 || i + 1 == c->n)
            out("\n");
        else
            for (; l < w; l++)
                uart_putc(' ');
    }
    redraw_fresh(e);
}

static void complete(Edit *e, const LineCompleter *lc)
{
    // 補う語: カーソルの前の空白の後から、カーソルまで
    int start = e->pos;
    while (start > 0 && e->buf[start - 1] != ' ')
        start--;
    bool first_word = true;
    for (int i = 0; i < start; i++)
        if (e->buf[i] != ' ')
            first_word = false;
    char word[LINE_EDIT_MAX];
    int wl = e->pos - start;
    memcpy(word, e->buf + start, (size_t)wl);
    word[wl] = '\0';

    Cands c = {0};
    const char *base = word;    // 候補と比べる部分（ファイル名ならディレクトリの後）
    if (first_word && !strchr(word, '/') && lc && lc->command_name) {
        for (int i = 0; lc->command_name(i); i++)
            if (!strncmp(lc->command_name(i), word, (size_t)wl))
                cands_add(&c, lc->command_name(i), false);
    } else {
        char dir[LINE_EDIT_MAX];
        const char *slash = strrchr(word, '/');
        if (slash) {
            int dl = (int)(slash - word);
            snprintf(dir, sizeof(dir), "%.*s", dl ? dl : 1, word);
            base = slash + 1;
        } else {
            strcpy(dir, ".");
        }
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *ent;
            size_t bl = strlen(base);
            while ((ent = readdir(d))) {
                if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
                    continue;
                if (base[0] != '.' && ent->d_name[0] == '.')
                    continue;   // 「.」で始まるものは、「.」を打ったときだけ
                if (strncasecmp(ent->d_name, base, bl))
                    continue;   // FAT は大文字・小文字を区別しない
                char path[LINE_EDIT_MAX * 2];
                snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
                struct stat st;
                cands_add(&c, ent->d_name, stat(path, &st) == 0 && S_ISDIR(st.st_mode));
            }
            closedir(d);
        }
    }
    if (c.n == 0) {
        cands_free(&c);
        return;
    }
    int bl = (int)strlen(base);
    // 共通の部分（1 つなら全部）
    int common = (int)strlen(c.names[0]);
    for (int i = 1; i < c.n; i++) {
        int k = 0;
        while (k < common && c.names[i][k] && c.names[0][k] == c.names[i][k])
            k++;
        common = k;
    }
    if (c.n == 1 || common > bl) {
        // 打った部分を、候補の綴り（大文字・小文字）に合わせて置き換え、残りを足す
        char add[LINE_EDIT_MAX + 2];
        int n = snprintf(add, sizeof(add), "%.*s", common, c.names[0]);
        if (c.n == 1 && n < (int)sizeof(add) - 1)
            add[n++] = c.dir[0] ? '/' : ' ', add[n] = '\0';
        int from = e->pos - bl;
        bool same = !strncmp(e->buf + from, add, (size_t)bl);
        if (same) {
            insert(e, add + bl, n - bl);
        } else {
            delete_range(e, from, e->pos);
            insert(e, add, n);
        }
    } else {
        list_cands(e, &c);
    }
    cands_free(&c);
}

// ---- 1 行を読む ----

int line_edit(const char *prompt, char *buf, int size, const LineCompleter *lc, bool use_history)
{
    Edit e = {.prompt = prompt, .buf = buf, .size = size};
    buf[0] = '\0';
    e.plen = width_of(prompt, (int)strlen(prompt));
    out(prompt);
    e.shown_cur = e.shown_total = e.plen;
    e.echoed = true;
    if (use_history)
        history_load();
    int hidx = nhistory;            // 履歴のどれを出しているか（nhistory は今打っている行）
    char *editing = nullptr;        // 履歴を見る前に打っていた行

    for (;;) {
        int c = read_key();
        // UTF-8 の文字は、続きのバイトもまとめて 1 文字として入れる
        if (c >= 0xC0 && c < 0x100) {
            char s[4] = {(char)c};
            int l = char_len((unsigned char)c);
            int n = 1;
            for (; n < l; n++) {
                int b = read_byte_soon();
                if (b < 0 || (b & 0xC0) != 0x80)
                    break;
                s[n] = (char)b;
            }
            if (n == l)
                insert(&e, s, l);
            continue;
        }
        switch (c) {
        case '\r':
        case '\n':
            if (e.pos != e.len) {
                e.pos = e.len;
                refresh(&e);
            }
            out("\n");
            free(editing);
            return e.len;
        case 0x03:  // Ctrl-C
            out("^C\n");
            buf[0] = '\0';
            free(editing);
            return LINE_EDIT_CANCEL;
        case 0x04:  // Ctrl-D
            if (e.len == 0) {
                out("\n");
                free(editing);
                return LINE_EDIT_EOF;
            }
            delete_range(&e, e.pos, next_char(&e, e.pos));
            break;
        case 0x08:  // Backspace（Ctrl-H）
        case 0x7F:
            if (e.pos > 0) {
                if (e.pos == e.len && !e.maxrows && width_of(buf + prev_char(&e, e.pos), e.pos - prev_char(&e, e.pos)) == 1 &&
                    e.shown_total < (e.cols ? e.cols : 80)) {
                    // 折り返していない行の終わりの半角を消すだけ: 今までと同じく「\b \b」
                    e.pos = e.len = prev_char(&e, e.pos);
                    buf[e.len] = '\0';
                    out("\b \b");
                    e.shown_cur = e.shown_total = e.plen + width_of(buf, e.len);
                    e.echoed = true;
                } else {
                    delete_range(&e, prev_char(&e, e.pos), e.pos);
                }
            }
            break;
        case K_DELETE:
            delete_range(&e, e.pos, next_char(&e, e.pos));
            break;
        case K_LEFT:
        case 0x02:  // Ctrl-B
            if (e.pos > 0) {
                e.pos = prev_char(&e, e.pos);
                refresh(&e);
            }
            break;
        case K_RIGHT:
        case 0x06:  // Ctrl-F
            if (e.pos < e.len) {
                e.pos = next_char(&e, e.pos);
                refresh(&e);
            }
            break;
        case K_HOME:
        case 0x01:  // Ctrl-A
            e.pos = 0;
            refresh(&e);
            break;
        case K_END:
        case 0x05:  // Ctrl-E
            e.pos = e.len;
            refresh(&e);
            break;
        case K_WORD_LEFT:
            e.pos = word_left(&e, e.pos);
            refresh(&e);
            break;
        case K_WORD_RIGHT:
            e.pos = word_right(&e, e.pos);
            refresh(&e);
            break;
        case 0x17:  // Ctrl-W
        case K_WORD_BACKSPACE:
            delete_range(&e, word_left(&e, e.pos), e.pos);
            break;
        case K_WORD_DELETE:
            delete_range(&e, e.pos, word_right(&e, e.pos));
            break;
        case 0x15:  // Ctrl-U
            delete_range(&e, 0, e.pos);
            break;
        case 0x0B:  // Ctrl-K
            delete_range(&e, e.pos, e.len);
            break;
        case 0x0C:  // Ctrl-L
            out("\x1b[H\x1b[2J");
            redraw_fresh(&e);
            break;
        case '\t':
            complete(&e, lc);
            break;
        case K_UP:
        case 0x10:  // Ctrl-P
        case K_DOWN:
        case 0x0E:  // Ctrl-N
            if (!use_history)
                break;
            {
                bool up = c == K_UP || c == 0x10;
                int next = hidx + (up ? -1 : 1);
                if (next < 0 || next > nhistory)
                    break;
                if (hidx == nhistory) {
                    free(editing);
                    editing = strdup(buf);
                }
                hidx = next;
                set_line(&e, hidx == nhistory ? (editing ? editing : "") : history[hidx]);
            }
            break;
        default:
            if (c >= 0x20 && c < 0x80) {
                char ch = (char)c;
                insert(&e, &ch, 1);
            }
            break;
        }
    }
}
