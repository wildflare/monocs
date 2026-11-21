// 展開（inflate）。RFC 1951（DEFLATE）・RFC 1952（gzip）・RFC 1950（zlib）を見て自分で書いたもの
//
// - 入力は読む関数から 16KB ずつ、ビットは 64 ビットの入れ物に下位から詰める（DEFLATE はバイトの下位ビットから）
// - 出力は 32KB の窓（前に出したものを写す「距離」は 32768 まで）に書き、いっぱいになるたびに書く関数に渡す
// - ハフマン符号は正準（長さだけで決まる）。9 ビットまでは表を 1 回引き、長いものは 1 ビットずつ足して探す
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "monocs_inflate.h"

#define IN_SIZE     16384
#define WINDOW      32768
#define FAST_BITS   9
#define MAX_BITS    15

typedef struct {
    uint16_t fast[1 << FAST_BITS];  // 0 = 長い符号。ほかは（記号 << 4）| 長さ
    uint16_t count[MAX_BITS + 1];   // 長さごとの符号の数
    uint16_t symbol[288];           // 符号の順に並べた記号
} Huffman;

typedef struct {
    MonocsInflateRead read;
    void *read_ctx;
    MonocsInflateWrite write;
    void *write_ctx;
    const char *error;
    int status;                 // 0 か、返すエラー
    // 入力
    uint8_t in[IN_SIZE];
    int in_pos, in_len;
    bool in_eof;
    uint64_t bits;
    int nbits;
    // 出力
    uint8_t window[WINDOW];
    uint32_t wpos;              // 窓の中の次に書く位置
    uint64_t total;             // 出したバイト数（今の塊）
    bool wrapped;               // 窓がひと回りした（距離の検査に使う）
    // 検査
    int format;
    uint32_t crc, adler_a, adler_b;
    Huffman lit, dist;
} Inflate;

static uint32_t crc_table[256];

static void crc_init(void)
{
    if (crc_table[1])
        return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

unsigned monocs_crc32(unsigned crc, const void *data, unsigned long len)
{
    crc_init();
    const uint8_t *p = data;
    uint32_t c = ~crc;
    while (len--)
        c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static int fail(Inflate *z, int status, const char *msg)
{
    if (!z->status) {
        z->status = status;
        z->error = msg;
    }
    return -1;
}

// ---- 入力 ----
static bool refill(Inflate *z)
{
    if (z->in_eof || z->status)
        return false;
    int n = z->read(z->read_ctx, z->in, IN_SIZE);
    if (n < 0) {
        fail(z, -2, "read error");
        return false;
    }
    if (n == 0) {
        z->in_eof = true;
        return false;
    }
    z->in_pos = 0;
    z->in_len = n;
    return true;
}

// ビットの入れ物を 32 ビット以上にする（入力が尽きたら、あるだけ）
static void need_bits(Inflate *z)
{
    while (z->nbits <= 56) {
        if (z->in_pos == z->in_len && !refill(z))
            return;
        z->bits |= (uint64_t)z->in[z->in_pos++] << z->nbits;
        z->nbits += 8;
    }
}

static uint32_t get_bits(Inflate *z, int n)
{
    if (z->nbits < n) {
        need_bits(z);
        if (z->nbits < n) {
            fail(z, -1, "unexpected end of data");
            return 0;
        }
    }
    uint32_t v = (uint32_t)(z->bits & ((1ull << n) - 1));
    z->bits >>= n;
    z->nbits -= n;
    return v;
}

// バイトの境目まで捨てる
static void align_byte(Inflate *z)
{
    int drop = z->nbits & 7;
    z->bits >>= drop;
    z->nbits -= drop;
}

static int get_byte(Inflate *z)
{
    return (int)get_bits(z, 8);
}

// ---- 出力 ----
static void check_update(Inflate *z, const uint8_t *p, uint32_t n)
{
    if (z->format == MONOCS_INFLATE_GZIP) {
        z->crc = monocs_crc32(z->crc, p, n);
    } else if (z->format == MONOCS_INFLATE_ZLIB) {
        uint32_t a = z->adler_a, b = z->adler_b;
        while (n) {     // 5552 バイトまでは 32 ビットであふれない（RFC 1950 の Adler-32）
            uint32_t k = n < 5552 ? n : 5552;
            n -= k;
            while (k--) {
                a += *p++;
                b += a;
            }
            a %= 65521;
            b %= 65521;
        }
        z->adler_a = a;
        z->adler_b = b;
    }
}

// 窓の先頭から wpos までを書く関数に渡す
static void flush_window(Inflate *z)
{
    if (z->wpos == 0 || z->status)
        return;
    check_update(z, z->window, z->wpos);
    if (z->write(z->write_ctx, z->window, (int)z->wpos) != 0)
        fail(z, -3, "write error");
    z->total += z->wpos;
    z->wpos = 0;
    z->wrapped = true;
}

static inline void put_byte(Inflate *z, uint8_t c)
{
    z->window[z->wpos++] = c;
    if (z->wpos == WINDOW)
        flush_window(z);
}

// ---- ハフマン符号 ----
// lengths（記号ごとの符号の長さ。0 は使わない）から表を作る。多すぎる・足りない割り当ては誤り
// （長さ 1 の符号が 1 つだけ、は認める: 距離の符号が 1 つのとき。RFC 1951 3.2.7）
static bool build(Huffman *h, const uint8_t *lengths, int n)
{
    uint16_t offs[MAX_BITS + 1];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++)
        h->count[lengths[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int len = 1; len <= MAX_BITS; len++) {
        left = (left << 1) - h->count[len];
        if (left < 0)
            return false;       // 割り当てすぎ
    }
    offs[1] = 0;
    for (int len = 1; len < MAX_BITS; len++)
        offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (lengths[i])
            h->symbol[offs[lengths[i]]++] = (uint16_t)i;
    // 速い表: 符号（上位ビットから）をひっくり返した値で引く
    memset(h->fast, 0, sizeof h->fast);
    int code = 0, index = 0;
    for (int len = 1; len <= FAST_BITS; len++) {
        for (int k = 0; k < h->count[len]; k++, code++, index++) {
            int rev = 0;
            for (int b = 0; b < len; b++)
                rev |= ((code >> b) & 1) << (len - 1 - b);
            for (int fill = rev; fill < (1 << FAST_BITS); fill += 1 << len)
                h->fast[fill] = (uint16_t)(h->symbol[index] << 4 | len);
        }
        code <<= 1;
    }
    return true;
}

static int decode(Inflate *z, const Huffman *h)
{
    if (z->nbits < MAX_BITS)
        need_bits(z);
    uint16_t e = h->fast[z->bits & ((1 << FAST_BITS) - 1)];
    if (e) {
        int len = e & 15;
        if (len > z->nbits)
            return fail(z, -1, "unexpected end of data");
        z->bits >>= len;
        z->nbits -= len;
        return e >> 4;
    }
    // 長い符号: 1 ビットずつ足して、その長さの符号の範囲に入るかを見る
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAX_BITS; len++) {
        if (len > z->nbits)
            return fail(z, -1, "unexpected end of data");
        code |= (int)(z->bits >> (len - 1)) & 1;
        int count = h->count[len];
        if (code - count < first) {
            z->bits >>= len;
            z->nbits -= len;
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return fail(z, -1, "invalid code");
}

// 長さ・距離の基準の値と、足すビットの数（RFC 1951 3.2.5）
static const uint16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                       1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
                                       9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// 圧縮されたブロックの中身（リテラル・長さと距離の組）
static void codes(Inflate *z)
{
    while (!z->status) {
        int sym = decode(z, &z->lit);
        if (sym < 0)
            return;
        if (sym < 256) {
            put_byte(z, (uint8_t)sym);
            continue;
        }
        if (sym == 256)
            return;
        sym -= 257;
        if (sym >= 29) {
            fail(z, -1, "invalid length code");
            return;
        }
        uint32_t len = len_base[sym] + get_bits(z, len_extra[sym]);
        int ds = decode(z, &z->dist);
        if (ds < 0)
            return;
        if (ds >= 30) {
            fail(z, -1, "invalid distance code");
            return;
        }
        uint32_t dist = dist_base[ds] + get_bits(z, dist_extra[ds]);
        if (z->status)
            return;
        if (dist > z->wpos && !z->wrapped) {
            fail(z, -1, "distance too far back");
            return;
        }
        uint32_t from = (z->wpos + WINDOW - dist) & (WINDOW - 1);
        while (len--) {
            put_byte(z, z->window[from]);
            from = (from + 1) & (WINDOW - 1);
        }
    }
}

static void stored(Inflate *z)
{
    align_byte(z);
    uint32_t len = get_bits(z, 16), nlen = get_bits(z, 16);
    if (z->status)
        return;
    if ((len ^ nlen) != 0xFFFF) {
        fail(z, -1, "invalid stored block");
        return;
    }
    while (len && !z->status) {
        if (z->nbits >= 8) {        // ビットの入れ物に残っているバイトから
            put_byte(z, (uint8_t)get_bits(z, 8));
            len--;
            continue;
        }
        if (z->in_pos == z->in_len && !refill(z)) {
            fail(z, -1, "unexpected end of data");
            return;
        }
        uint32_t n = (uint32_t)(z->in_len - z->in_pos);
        if (n > len)
            n = len;
        if (n > WINDOW - z->wpos)
            n = WINDOW - z->wpos;
        memcpy(z->window + z->wpos, z->in + z->in_pos, n);
        z->in_pos += (int)n;
        z->wpos += n;
        len -= n;
        if (z->wpos == WINDOW)
            flush_window(z);
    }
}

static void fixed_tables(Inflate *z)
{
    uint8_t l[288];
    memset(l, 8, 144);
    memset(l + 144, 9, 112);
    memset(l + 256, 7, 24);
    memset(l + 280, 8, 8);
    build(&z->lit, l, 288);
    memset(l, 5, 30);
    build(&z->dist, l, 30);
}

static void dynamic_tables(Inflate *z)
{
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen = (int)get_bits(z, 5) + 257, ndist = (int)get_bits(z, 5) + 1, ncode = (int)get_bits(z, 4) + 4;
    if (z->status)
        return;
    if (nlen > 286 || ndist > 30) {
        fail(z, -1, "too many length or distance codes");
        return;
    }
    uint8_t l[288 + 32] = {0};
    for (int i = 0; i < ncode; i++)
        l[order[i]] = (uint8_t)get_bits(z, 3);
    Huffman *cl = &z->dist;     // 符号の長さの符号（一時的に dist の場所を使う）
    if (z->status || !build(cl, l, 19)) {
        fail(z, -1, "invalid code lengths");
        return;
    }
    uint8_t lengths[286 + 30];
    int i = 0;
    while (i < nlen + ndist) {
        int sym = decode(z, cl);
        if (sym < 0)
            return;
        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
            continue;
        }
        int rep, val = 0;
        if (sym == 16) {
            if (i == 0) {
                fail(z, -1, "invalid code lengths (repeat with no previous length)");
                return;
            }
            val = lengths[i - 1];
            rep = 3 + (int)get_bits(z, 2);
        } else if (sym == 17) {
            rep = 3 + (int)get_bits(z, 3);
        } else {
            rep = 11 + (int)get_bits(z, 7);
        }
        if (z->status)
            return;
        if (i + rep > nlen + ndist) {
            fail(z, -1, "invalid code lengths (too many)");
            return;
        }
        while (rep--)
            lengths[i++] = (uint8_t)val;
    }
    if (lengths[256] == 0) {
        fail(z, -1, "no end-of-block code");
        return;
    }
    if (!build(&z->lit, lengths, nlen) || !build(&z->dist, lengths + nlen, ndist))
        fail(z, -1, "invalid Huffman codes");
}

// DEFLATE の流れ 1 つ（最後のブロックまで）
static void deflate_stream(Inflate *z)
{
    z->wpos = 0;
    z->wrapped = false;
    z->total = 0;
    for (bool last = false; !last && !z->status;) {
        last = get_bits(z, 1) != 0;
        uint32_t type = get_bits(z, 2);
        if (z->status)
            break;
        if (type == 0) {
            stored(z);
        } else if (type == 1) {
            fixed_tables(z);
            codes(z);
        } else if (type == 2) {
            dynamic_tables(z);
            if (!z->status)
                codes(z);
        } else {
            fail(z, -1, "invalid block type");
        }
    }
    flush_window(z);
}

static uint32_t get_le(Inflate *z, int bytes)
{
    uint32_t v = 0;
    for (int i = 0; i < bytes; i++)
        v |= (uint32_t)get_byte(z) << (8 * i);
    return v;
}

// gzip の頭の残り（始めの 2 バイト 1f 8b は読んである）。RFC 1952 2.3
static void gzip_header(Inflate *z)
{
    int cm = get_byte(z), flg = get_byte(z);
    get_le(z, 4);       // MTIME
    get_byte(z);        // XFL
    get_byte(z);        // OS
    if (z->status)
        return;
    if (cm != 8 || (flg & 0xE0)) {
        fail(z, -1, "unsupported gzip header");
        return;
    }
    if (flg & 4) {      // FEXTRA
        uint32_t n = get_le(z, 2);
        while (n-- && !z->status)
            get_byte(z);
    }
    if (flg & 8)        // FNAME
        while (!z->status && get_byte(z) != 0)
            ;
    if (flg & 16)       // FCOMMENT
        while (!z->status && get_byte(z) != 0)
            ;
    if (flg & 2)        // FHCRC
        get_le(z, 2);
}

// まだ入力があるか（次の 1 バイトを読めるか）
static bool more_input(Inflate *z)
{
    if (z->nbits >= 8)
        return true;
    need_bits(z);
    return z->nbits >= 8;
}

int monocs_inflate(int format, MonocsInflateRead read, void *read_ctx, MonocsInflateWrite write, void *write_ctx,
                 const char **error)
{
    Inflate *z = calloc(1, sizeof *z);
    if (!z) {
        if (error)
            *error = "out of memory";
        return -4;
    }
    z->read = read;
    z->read_ctx = read_ctx;
    z->write = write;
    z->write_ctx = write_ctx;
    if (format == MONOCS_INFLATE_AUTO) {
        // 始めの 2 バイトを見る（ビットの入れ物に置いたまま）: gzip は 1f 8b、zlib は「下位 4 ビットが 8、
        // 2 バイトを大きいほうからの数として 31 で割り切れる」。どちらでもなければ DEFLATE だけ
        need_bits(z);
        uint32_t b0 = (uint32_t)z->bits & 0xFF, b1 = (uint32_t)(z->bits >> 8) & 0xFF;
        format = z->nbits >= 16 && b0 == 0x1F && b1 == 0x8B ? MONOCS_INFLATE_GZIP
                 : z->nbits >= 16 && (b0 & 15) == 8 && (b0 * 256 + b1) % 31 == 0 ? MONOCS_INFLATE_ZLIB
                 : MONOCS_INFLATE_RAW;
    }
    z->format = format;
    if (format == MONOCS_INFLATE_GZIP) {
        // 塊が続けて並んでいてもよい（RFC 1952 2.2）。2 つ目からは、1f 8b で始まらなければ終わり（後ろの詰め物）
        for (int member = 0; !z->status; member++) {
            if (member > 0 && !more_input(z))
                break;
            int m0 = get_byte(z), m1 = get_byte(z);
            if (z->status || m0 != 0x1F || m1 != 0x8B) {
                if (member == 0)
                    fail(z, -1, "not in gzip format");
                else
                    z->status = 0, z->error = nullptr;    // 後ろの余り（0 の詰め物など）は無視する（gzip と同じ）
                break;
            }
            gzip_header(z);
            z->crc = 0;
            deflate_stream(z);
            align_byte(z);
            uint32_t crc = get_le(z, 4), isize = get_le(z, 4);
            if (!z->status && (crc != z->crc || isize != (uint32_t)z->total))
                fail(z, -1, "crc or length error");
        }
    } else if (format == MONOCS_INFLATE_ZLIB) {
        int cmf = get_byte(z), flg = get_byte(z);
        if (!z->status && ((cmf & 15) != 8 || (cmf >> 4) > 7 || (cmf * 256 + flg) % 31 || (flg & 0x20)))
            fail(z, -1, "invalid zlib header");
        z->adler_a = 1;
        z->adler_b = 0;
        if (!z->status)
            deflate_stream(z);
        align_byte(z);
        uint32_t want = 0;
        for (int i = 0; i < 4; i++)
            want = want << 8 | (uint32_t)get_byte(z);
        if (!z->status && want != (z->adler_b << 16 | z->adler_a))
            fail(z, -1, "adler32 error");
    } else {
        deflate_stream(z);
    }
    int status = z->status;
    if (error)
        *error = z->error;
    free(z);
    return status;
}
