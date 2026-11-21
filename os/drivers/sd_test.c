// SDHOST のドライバを、ファイルシステムを壊さずに試す（シェルの sdtest。os/fs/vfs.c の vfs_exclusive の中で）
// 起動とファイルシステムは EMMC で動かしたまま、試すときだけ GPIO48〜53 を SDHOST に切り替え、区画の外の
// 空いているセクタ（MBR の区画の終わりから 32MB 先）だけを読み書きする。終わったら EMMC に戻す。
// 試す場所の元の中身は、先に EMMC で読んでおき、最後に書き戻す。
// 1. EMMC で模様を書き、SDHOST でいろいろな数・境界で読んで比べる（読み出しを試す）
// 2. SDHOST で別の模様をいろいろな数・境界で書き、SDHOST で読んで比べ、EMMC に戻してもう一度比べる（書き込み）
// 模様は語ごとに違う値なので、ずれていれば何語ずれたかがわかる。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sd.h"
#include "sd_backend.h"
#include "timer.h"

#define TEST_SECTORS    4096        // 2MB
#define TEST_GAP        65536       // 区画の終わりから 32MB 先から
#define MAX_REPORT      6

static uint32_t pattern_word(uint32_t seed, uint32_t sector, uint32_t i)
{
    uint32_t x = (sector * 128 + i) * 2654435761u ^ seed;
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    return x;
}

static void fill(uint8_t *buf, uint32_t seed, uint32_t sector, uint32_t count)
{
    for (uint32_t s = 0; s < count; s++)
        for (uint32_t i = 0; i < 128; i++) {
            uint32_t w = pattern_word(seed, sector + s, i);
            memcpy(buf + s * 512 + i * 4, &w, 4);
        }
}

static int errors_reported;

// buf（sector から count セクタ）が模様と同じか。違えば最初のところと、何語ずれているかを表示する
static bool check(const char *what, const uint8_t *buf, uint32_t seed, uint32_t base, uint32_t sector, uint32_t count)
{
    for (uint32_t s = 0; s < count; s++)
        for (uint32_t i = 0; i < 128; i++) {
            uint32_t got;
            memcpy(&got, buf + s * 512 + i * 4, 4);
            if (got == pattern_word(seed, sector + s, i))
                continue;
            if (errors_reported++ < MAX_REPORT) {
                int shift = 0;
                for (int d = -16; d <= 16 && !shift; d++) {
                    int64_t k = (int64_t)(sector + s - base) * 128 + i + d;
                    if (d && k >= 0 && k < (int64_t)TEST_SECTORS * 128 &&
                        got == pattern_word(seed, base + (uint32_t)(k / 128), (uint32_t)(k % 128)))
                        shift = d;
                }
                printf("  %s: mismatch at sector +%u word %u (got %08x)", what, sector + s - base, i, got);
                if (shift)
                    printf(", shifted by %+d words", shift);
                printf("\n");
            }
            return false;
        }
    return true;
}

// 読み書きする（数, ずらし）の組。ずらしはバッファの番地を 4 バイト境界から外す分
static const struct { uint32_t count, misalign; } sizes[] = {
    {1, 0}, {2, 0}, {3, 1}, {8, 0}, {17, 2}, {1, 3}, {64, 0}, {1, 0}, {128, 1}, {511, 0}, {1, 0},
    {512, 0}, {600, 3}, {7, 0}, {1000, 0}, {2, 0},
};

typedef bool (*rw_fn)(uint32_t, uint32_t, void *);

static bool sdhost_read_fn(uint32_t s, uint32_t n, void *b) { return sdhost_sd_read(s, n, b); }

// 試す場所を sizes の組の順に区切りながら、端から端まで読む（または書く）
static int sweep(bool write, uint32_t seed, uint32_t base, uint8_t *raw, const char *what)
{
    int bad = 0;
    uint32_t pos = 0;
    for (unsigned k = 0; pos < TEST_SECTORS; k = (k + 1) % (sizeof sizes / sizeof sizes[0])) {
        uint32_t n = sizes[k].count;
        if (n > TEST_SECTORS - pos)
            n = TEST_SECTORS - pos;
        uint8_t *buf = raw + sizes[k].misalign;
        if (write) {
            fill(buf, seed, base + pos, n);
            if (!sdhost_sd_write(base + pos, n, buf)) {
                printf("  %s: write error at +%u (%u sectors)\n", what, pos, n);
                bad++;
            }
        } else {
            memset(buf, 0, n * 512);
            if (!sdhost_read_fn(base + pos, n, buf)) {
                printf("  %s: read error at +%u (%u sectors)\n", what, pos, n);
                bad++;
            } else if (!check(what, buf, seed, base, base + pos, n)) {
                bad++;
            }
        }
        pos += n;
    }
    return bad;
}

static bool back_to_emmc(void)
{
    sdhost_sd_release();
    if (emmc_sd_init())
        return true;
    printf("sdtest: *** EMMC re-init failed: do not write files, reboot ***\n");
    return false;
}

int sd_selftest(int rounds)
{
    if (strcmp(sd_controller(), "EMMC") != 0) {
        printf("sdtest: needs the kernel running on EMMC (now %s)\n", sd_controller());
        return 1;
    }
    uint8_t *mbr = malloc(512);
    uint8_t *saved = malloc(TEST_SECTORS * 512);
    uint8_t *raw = malloc(TEST_SECTORS * 512 + 16);
    int result = 1;
    if (!mbr || !saved || !raw) {
        printf("sdtest: out of memory\n");
        goto out;
    }
    if (!emmc_sd_read(0, 1, mbr) || mbr[510] != 0x55 || mbr[511] != 0xAA) {
        printf("sdtest: cannot read the MBR\n");
        goto out;
    }
    uint32_t end = 0;
    for (int p = 0; p < 4; p++) {
        const uint8_t *e = mbr + 446 + 16 * p;
        uint32_t lba, size;
        memcpy(&lba, e + 8, 4);
        memcpy(&size, e + 12, 4);
        if (e[4] && lba + size > end)
            end = lba + size;
    }
    uint32_t base = (end + TEST_GAP + 2047) & ~2047U;
    printf("sdtest: partitions end at sector %u, testing sectors %u-%u (%u rounds)\n",
           end, base, base + TEST_SECTORS - 1, rounds);
    if (!emmc_sd_read(base, TEST_SECTORS, saved)) {
        printf("sdtest: EMMC cannot read the test area (card too small?)\n");
        goto out;
    }

    int total_bad = 0;
    errors_reported = 0;
    for (int r = 0; r < rounds; r++) {
        uint32_t seed_a = 0x5D000000u + (uint32_t)r * 2, seed_b = seed_a + 1;
        int bad_read = 0, bad_write = 0, bad_cross = 0;

        // 1. EMMC で書き、SDHOST で読む
        fill(raw, seed_a, base, TEST_SECTORS);
        if (!emmc_sd_write(base, TEST_SECTORS, raw)) {
            printf("sdtest: EMMC write failed\n");
            break;
        }
        uint64_t t0 = timer_now_us();
        if (!sdhost_sd_init()) {
            printf("sdtest: SDHOST init failed\n");
            back_to_emmc();
            break;
        }
        uint64_t t1 = timer_now_us();
        bad_read = sweep(false, seed_a, base, raw, "read");
        uint64_t t2 = timer_now_us();

        // 2. SDHOST で書き、SDHOST で読む
        bad_write = sweep(true, seed_b, base, raw, "write");
        uint64_t t3 = timer_now_us();
        bad_write += sweep(false, seed_b, base, raw, "write+read");

        // 3. EMMC に戻して読む
        if (!back_to_emmc())
            return 1;
        memset(raw, 0, TEST_SECTORS * 512);
        if (!emmc_sd_read(base, TEST_SECTORS, raw) || !check("write (EMMC)", raw, seed_b, base, base, TEST_SECTORS))
            bad_cross++;

        // 速さ: 2MB を 1 回で読み書きする（SDHOST は 511 セクタずつに分かれる）
        uint64_t b0 = timer_now_us();
        bool bench_ok = emmc_sd_write(base, TEST_SECTORS, raw);
        uint64_t b1 = timer_now_us();
        bench_ok &= emmc_sd_read(base, TEST_SECTORS, raw);
        uint64_t b2 = timer_now_us();
        bench_ok &= sdhost_sd_init();
        uint64_t b3 = timer_now_us();
        bench_ok &= sdhost_sd_write(base, TEST_SECTORS, raw);
        uint64_t b4 = timer_now_us();
        bench_ok &= sdhost_sd_read(base, TEST_SECTORS, raw);
        uint64_t b5 = timer_now_us();
        if (!back_to_emmc())
            return 1;
        printf("  2MB at once: EMMC write %llu ms, read %llu ms; SDHOST write %llu ms, read %llu ms%s\n",
               (unsigned long long)(b1 - b0) / 1000, (unsigned long long)(b2 - b1) / 1000,
               (unsigned long long)(b4 - b3) / 1000, (unsigned long long)(b5 - b4) / 1000,
               bench_ok ? "" : " (error)");

        uint32_t stale, block_to;
        sdhost_debug(&stale, &block_to);
        printf("round %d: read %s, write %s, EMMC check %s  (init %llu ms, read 2MB %llu ms, write 2MB %llu ms; "
               "stale words %u, block timeouts %u)\n",
               r + 1, bad_read ? "NG" : "ok", bad_write ? "NG" : "ok", bad_cross ? "NG" : "ok",
               (unsigned long long)(t1 - t0) / 1000, (unsigned long long)(t2 - t1) / 1000,
               (unsigned long long)(t3 - t2) / 1000, stale, block_to);
        total_bad += bad_read + bad_write + bad_cross;
    }

    if (!emmc_sd_write(base, TEST_SECTORS, saved))
        printf("sdtest: could not restore the test area (it is outside the partitions)\n");
    printf("sdtest: %s\n", total_bad ? "FAILED" : "all ok");
    result = total_bad ? 1 : 0;
out:
    free(mbr);
    free(saved);
    free(raw);
    return result;
}

// ---- High Speed（50MHz）を試す（2026-11-21。sdtest hs [N]）----
// SDHOST で動いているまま試す（EMMC は Wi-Fi の SDIO に使っていて、Wi-Fi だけでつないでいるときに切り替えられない。
// 区画はカードの終わりまで広げたので、区画の外の空きもない）。
// 試す場所: FAT32 の区画の最後の空きクラスタ（TEST_SECTORS 分。FAT の項目がすべて 0 であることを確かめる）。元の中身は
// 25MHz で読んでおき、最後に書き戻して読み直す。High Speed に切り替えたカードは 25MHz でも動くので、比べる相手は 25MHz。
// 1. 区画の頭（予約・FAT・データの始め、REF_SECTORS）を 25MHz でまとめて読み、50MHz でいろいろな数・境界で読んで比べる
// 2. 50MHz で模様をいろいろな数・境界で書き、50MHz で読んで比べ、25MHz でもう一度まとめて読んで比べる

#define REF_SECTORS 16384       // 8MB

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// FAT32 の区画の最後の空きクラスタの範囲（TEST_SECTORS セクタ）を探す。区画の頭を *part に
static bool find_free_tail(uint8_t *sec, uint32_t *base, uint32_t *part)
{
    if (!sdhost_sd_read(0, 1, sec) || sec[510] != 0x55 || sec[511] != 0xAA) {
        printf("sdtest hs: cannot read the MBR\n");
        return false;
    }
    uint32_t lba = le32(sec + 446 + 8), psize = le32(sec + 446 + 12);
    if (!sdhost_sd_read(lba, 1, sec) || sec[510] != 0x55 || sec[511] != 0xAA || le16(sec + 11) != 512) {
        printf("sdtest hs: cannot read the FAT boot sector\n");
        return false;
    }
    uint32_t spc = sec[13], rsvd = le16(sec + 14), nfats = sec[16], totsec = le32(sec + 32), fatsz = le32(sec + 36);
    if (!spc || !nfats || !fatsz || le16(sec + 22) != 0 || totsec > psize || TEST_SECTORS % spc) {
        printf("sdtest hs: not a FAT32 volume this test understands\n");
        return false;
    }
    uint32_t data = lba + rsvd + nfats * fatsz;
    uint32_t nclust = (totsec - rsvd - nfats * fatsz) / spc;
    uint32_t k = TEST_SECTORS / spc;
    if (nclust < 2 * k) {
        printf("sdtest hs: the volume is too small\n");
        return false;
    }
    uint32_t first = nclust + 2 - k;    // クラスタの番号は 2 から
    // FAT（1 つ目）の項目が空き（下の 28 ビットが 0）か
    for (uint32_t c = first; c < first + k; c++) {
        uint32_t s = lba + rsvd + c * 4 / 512;
        if (c == first || c * 4 % 512 == 0) {
            if (!sdhost_sd_read(s, 1, sec)) {
                printf("sdtest hs: cannot read the FAT\n");
                return false;
            }
        }
        if (le32(sec + c * 4 % 512) & 0x0FFFFFFF) {
            printf("sdtest hs: cluster %u near the end of the volume is in use; not testing\n", c);
            return false;
        }
    }
    *base = data + (first - 2) * spc;
    *part = lba;
    return true;
}

static bool set_clock_or_say(bool high)
{
    if (sdhost_set_high_clock(high))
        return true;
    printf("sdtest hs: cannot %s\n", high ? "switch the card to high speed (not supported?)" : "go back to 25 MHz");
    return false;
}

int sd_selftest_hs(int rounds)
{
    if (strcmp(sd_controller(), "SDHOST") != 0) {
        printf("sdtest hs: needs the kernel running on SDHOST (now %s)\n", sd_controller());
        return 1;
    }
    bool was_high = sdhost_clock_hz() > 30000000;
    uint8_t *sec = malloc(512), *saved = malloc(TEST_SECTORS * 512), *raw = malloc(TEST_SECTORS * 512 + 16);
    uint8_t *ref = malloc(REF_SECTORS * 512), *got = malloc(REF_SECTORS * 512 + 16);
    int result = 1;
    uint32_t base, part;
    if (!sec || !saved || !raw || !ref || !got) {
        printf("sdtest hs: out of memory\n");
        goto out;
    }
    if (!set_clock_or_say(false) || !find_free_tail(sec, &base, &part))
        goto out;
    printf("sdtest hs: testing free sectors %u-%u at the end of the volume (%u rounds)\n", base,
           base + TEST_SECTORS - 1, rounds);
    if (!sdhost_sd_read(base, TEST_SECTORS, saved) || !sdhost_sd_read(part, REF_SECTORS, ref)) {
        printf("sdtest hs: cannot read at 25 MHz\n");
        goto out;
    }
    if (!set_clock_or_say(true))
        goto out;
    printf("sdtest hs: card in high speed mode, clock %u Hz\n", (unsigned)sdhost_clock_hz());

    int total_bad = 0;
    errors_reported = 0;
    for (int r = 0; r < rounds; r++) {
        uint32_t seed = 0x48530000u + (uint32_t)r;
        int bad_ref = 0, bad_write = 0, bad_slow = 0;
        // 1. 使っている場所を 50MHz で、いろいろな数・境界で読み、25MHz で読んだものと比べる
        uint64_t t0 = timer_now_us();
        uint32_t pos = 0;
        for (unsigned k = (unsigned)r % (sizeof sizes / sizeof sizes[0]); pos < REF_SECTORS; k = (k + 1) % (sizeof sizes / sizeof sizes[0])) {
            uint32_t n = sizes[k].count;
            if (n > REF_SECTORS - pos)
                n = REF_SECTORS - pos;
            uint8_t *buf = got + sizes[k].misalign;
            memset(buf, 0, n * 512);
            if (!sdhost_sd_read(part + pos, n, buf) || memcmp(buf, ref + pos * 512, n * 512)) {
                if (errors_reported++ < MAX_REPORT)
                    printf("  read 50 MHz: differs from 25 MHz at +%u (%u sectors)\n", pos, n);
                bad_ref++;
            }
            pos += n;
        }
        uint64_t t1 = timer_now_us();
        // 2. 50MHz で書いて 50MHz で読む、25MHz でまとめて読む
        bad_write = sweep(true, seed, base, raw, "write 50 MHz");
        uint64_t t2 = timer_now_us();
        bad_write += sweep(false, seed, base, raw, "write+read 50 MHz");
        if (!set_clock_or_say(false))
            break;
        memset(raw, 0, TEST_SECTORS * 512);
        if (!sdhost_sd_read(base, TEST_SECTORS, raw) || !check("read 25 MHz", raw, seed, base, base, TEST_SECTORS))
            bad_slow++;
        // 速さ: 2MB をまとめて
        uint64_t b0 = timer_now_us();
        bool bench = sdhost_sd_read(base, TEST_SECTORS, raw);
        uint64_t b1 = timer_now_us();
        bench &= sdhost_sd_write(base, TEST_SECTORS, raw);
        uint64_t b2 = timer_now_us();
        if (!set_clock_or_say(true))
            break;
        uint64_t b3 = timer_now_us();
        bench &= sdhost_sd_read(base, TEST_SECTORS, raw);
        uint64_t b4 = timer_now_us();
        bench &= sdhost_sd_write(base, TEST_SECTORS, raw);
        uint64_t b5 = timer_now_us();
        uint32_t stale, block_to;
        sdhost_debug(&stale, &block_to);
        printf("round %d: read %s, write %s, 25 MHz check %s  (8MB read %llu ms, 2MB write %llu ms; 2MB at once: "
               "25 MHz read %llu write %llu ms, 50 MHz read %llu write %llu ms%s; stale words %u, block timeouts %u)\n",
               r + 1, bad_ref ? "NG" : "ok", bad_write ? "NG" : "ok", bad_slow ? "NG" : "ok",
               (unsigned long long)(t1 - t0) / 1000, (unsigned long long)(t2 - t1) / 1000,
               (unsigned long long)(b1 - b0) / 1000, (unsigned long long)(b2 - b1) / 1000,
               (unsigned long long)(b4 - b3) / 1000, (unsigned long long)(b5 - b4) / 1000, bench ? "" : " (error)",
               stale, block_to);
        total_bad += bad_ref + bad_write + bad_slow;
    }
    // 元に戻す（25MHz で書いて読み直す）
    if (!set_clock_or_say(false) || !sdhost_sd_write(base, TEST_SECTORS, saved) ||
        !sdhost_sd_read(base, TEST_SECTORS, raw) || memcmp(raw, saved, TEST_SECTORS * 512)) {
        printf("sdtest hs: *** could not restore the test area (free clusters; not used by files) ***\n");
        total_bad++;
    }
    printf("sdtest hs: %s\n", total_bad ? "FAILED" : "all ok");
    result = total_bad ? 1 : 0;
out:
    sdhost_set_high_clock(was_high);
    free(sec);
    free(saved);
    free(raw);
    free(ref);
    free(got);
    return result;
}
