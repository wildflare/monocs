// FatFs と SD カードドライバのつなぎ（物理ドライブ 0 = SD カード）
// 出典: FatFs の documents/doc/dinit.html ほか（disk_* の仕様）
//
// SD カード（実機）は、1 回の書き込み・読み出しのコマンドごとに待ちがある（書き込みは完了まで数 ms、
// 読み出しは約 0.3ms）。イメージ（tools/mkimg.sh の 64MB の FAT32）はクラスタが 1 セクタなので、
// FatFs は 1 セクタずつ読み書きし、ファイルを閉じるたびに FAT（2 つ）とディレクトリを書く。そこで:
//   書き込み: 書き込みを遅らせる（ライトバック。ユーザーと合意、2026-11-06）。まだ書いていないセクタの表
//            （最大 4MB）にためて、同じセクタの書き直しは表の中でまとめる。書き出すのは、最初の書き込みから
//            1 秒たったとき（diskio_writeback。ファイル操作のついでと、シェルが入力を待つ間）、表がいっぱいの
//            とき、sync・reboot・アプリの終了のとき。セクタ番号の順に並べ、続くものは 1 回の複数ブロックの
//            書き込みにする。電源が切れると、書き出す前の分（最大約 1 秒）は失われる
//   読み出し: 順に読むときの先読みと、飛び飛びの 1 セクタ（FAT・ディレクトリ）のキャッシュ。
//            まだ書いていないセクタは、表の中身を返す（読んだものの上に重ねる）
// FatFs を呼ぶのは VFS だけで、VFS のロックの中なので、ここはロックを取らない

#include "ff.h"
#include "diskio.h"
#include <stdlib.h>
#include <string.h>
#include "writeback.h"
#include "../drivers/sd.h"
#include "../drivers/timer.h"

static DSTATUS status = STA_NOINIT;

// ---- まだ書いていないセクタ（ライトバック） ----

#define WB_MAX          8192            // セクタ（4MB）
#define WB_HASH         16384           // 番号 → 表の位置（開番地法。2 の累乗）
#define WB_DELAY_US     1000000         // 最初の書き込みから書き出すまで
#define RUN_MAX         2048            // 1 回の書き込みのセクタ数（1MB）
static uint8_t wb_data[WB_MAX][SD_SECTOR_SIZE];
static LBA_t wb_sector[WB_MAX];
static UINT wb_count;
static int32_t wb_hash[WB_HASH];        // 表の位置 + 1（0 は空き）
static uint64_t wb_deadline;            // 0 なら、まだ書いていないものはない
static uint8_t run_buf[RUN_MAX * SD_SECTOR_SIZE];
static uint64_t wb_flushes, wb_rewrites;

static UINT wb_slot(LBA_t sector)
{
    return (UINT)((sector * 2654435761u) & (WB_HASH - 1));
}

static uint8_t *wb_lookup(LBA_t sector)
{
    if (!wb_count)
        return nullptr;
    for (UINT h = wb_slot(sector);; h = (h + 1) & (WB_HASH - 1)) {
        int32_t k = wb_hash[h];
        if (!k)
            return nullptr;
        if (wb_sector[k - 1] == sector)
            return wb_data[k - 1];
    }
}

static int cmp_sector(const void *a, const void *b)
{
    LBA_t x = wb_sector[*(const UINT *)a], y = wb_sector[*(const UINT *)b];
    return x < y ? -1 : x > y;
}

// すべて書き出す
static bool wb_flush(void)
{
    if (!wb_count)
        return true;
    static UINT order[WB_MAX];
    for (UINT i = 0; i < wb_count; i++)
        order[i] = i;
    qsort(order, wb_count, sizeof(UINT), cmp_sector);
    bool ok = true;
    for (UINT i = 0; i < wb_count;) {
        // 番号が続く間を 1 回の書き込みにする
        UINT n = 0;
        LBA_t start = wb_sector[order[i]];
        while (i + n < wb_count && n < RUN_MAX && wb_sector[order[i + n]] == start + n) {
            memcpy(run_buf + n * SD_SECTOR_SIZE, wb_data[order[i + n]], SD_SECTOR_SIZE);
            n++;
        }
        if (!sd_write((uint32_t)start, n, run_buf))
            ok = false;
        i += n;
    }
    wb_count = 0;
    memset(wb_hash, 0, sizeof(wb_hash));
    wb_deadline = 0;
    wb_flushes++;
    return ok;
}

static bool wb_put(LBA_t sector, const BYTE *data)
{
    uint8_t *p = wb_lookup(sector);
    if (p) {
        memcpy(p, data, SD_SECTOR_SIZE);
        wb_rewrites++;
        return true;
    }
    if (wb_count == WB_MAX && !wb_flush())
        return false;
    UINT k = wb_count++;
    wb_sector[k] = sector;
    memcpy(wb_data[k], data, SD_SECTOR_SIZE);
    UINT h = wb_slot(sector);
    while (wb_hash[h])
        h = (h + 1) & (WB_HASH - 1);
    wb_hash[h] = (int32_t)k + 1;
    if (!wb_deadline)
        wb_deadline = timer_now_us() + WB_DELAY_US;
    return true;
}

bool diskio_writeback(bool force)
{
    if (!wb_count || (!force && timer_now_us() < wb_deadline))
        return true;
    return wb_flush();
}

void diskio_writeback_info(unsigned *dirty, uint64_t *flushes, uint64_t *rewrites)
{
    *dirty = wb_count;
    *flushes = wb_flushes;
    *rewrites = wb_rewrites;
}

// ---- 先読みと、1 セクタの読み出しのキャッシュ ----

// 先読み: 前の読み出しの直後のセクタを読むとき（順に読んでいるとき）は、まとめて読み、
// 続く読み出しはそこから返す。まとめる量は 8 セクタから始めて、順に読み続ける間は倍にしていく（RA_MAX まで）。
// 小さなファイルやディレクトリで大きく読みすぎないように。飛び飛びの読み出しは先読みしない
#define RA_MAX      128     // セクタ（64KB）
#define RA_MIN      8
static uint8_t ra_buf[RA_MAX * SD_SECTOR_SIZE];
static LBA_t ra_start;
static UINT ra_count;
static LBA_t last_read_end;     // 前の読み出しの次のセクタ
static UINT ra_window = RA_MIN;  // 次に先読みする量

// 1 セクタずつの飛び飛びの読み出し（FAT・ディレクトリ）を覚えておく（直接マップ、2MB）。書き込みでは中身を新しくする
#define SC_ENTRIES  4096
static uint8_t sc_data[SC_ENTRIES][SD_SECTOR_SIZE];
static LBA_t sc_tag[SC_ENTRIES];    // セクタ番号 + 1（0 は空き）

static uint8_t *sc_lookup(LBA_t sector)
{
    UINT i = sector % SC_ENTRIES;
    return sc_tag[i] == sector + 1 ? sc_data[i] : nullptr;
}

static void sc_insert(LBA_t sector, const BYTE *data)
{
    UINT i = sector % SC_ENTRIES;
    sc_tag[i] = sector + 1;
    memcpy(sc_data[i], data, SD_SECTOR_SIZE);
}

static void ra_invalidate(LBA_t sector, UINT count)
{
    if (ra_count && sector < ra_start + ra_count && ra_start < sector + count)
        ra_count = 0;
}

void diskio_discard(void)
{
    wb_count = 0;
    memset(wb_hash, 0, sizeof(wb_hash));
    wb_deadline = 0;
    ra_count = 0;
    memset(sc_tag, 0, sizeof(sc_tag));
}

DSTATUS disk_status(BYTE pdrv)
{
    return pdrv == 0 ? status : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0)
        return STA_NOINIT;
    if (status & STA_NOINIT) {
        if (sd_init())
            status &= ~STA_NOINIT;
    }
    return status;
}

// SD カード（と先読み・キャッシュ）から読む。まだ書いていないセクタは考えない
static DRESULT read_media2(BYTE *buff, LBA_t sector, UINT count, bool sequential);
static DRESULT read_media(BYTE *buff, LBA_t sector, UINT count, bool sequential)
{
    DRESULT r = read_media2(buff, sector, count, sequential);
    // 1 セクタの読み出しは、先読みから返したものもセクタのキャッシュに入れる（2026-11-21）。クラスタが 4KB の
    // カード（8 セクタが続く）では、ディレクトリを端から読むと「順に読んでいる」とみなされて先読みの道を通り、
    // キャッシュに入らなかった。956 個の .o があるフォルダにファイルを作るたびに SD カードから読み直し、
    // C ライブラリのビルドで 147MB を読んでいた（クラスタが 1 セクタのときは、ディレクトリのセクタが飛び飛びで、
    // キャッシュに入っていた）。まだ書いていないセクタはここまで来ない（disk_read が先に返す）
    if (r == RES_OK && count == 1)
        sc_insert(sector, buff);
    return r;
}

static DRESULT read_media2(BYTE *buff, LBA_t sector, UINT count, bool sequential)
{
    if (count == 1) {
        uint8_t *c = sc_lookup(sector);
        if (c) {
            memcpy(buff, c, SD_SECTOR_SIZE);
            return RES_OK;
        }
    }
    // 先読みした中にすべてあれば、そこから返す
    if (ra_count && sector >= ra_start && sector + count <= ra_start + ra_count) {
        memcpy(buff, ra_buf + (sector - ra_start) * SD_SECTOR_SIZE, count * SD_SECTOR_SIZE);
        return RES_OK;
    }
    if (!sequential || count >= RA_MAX) {
        return sd_read(sector, count, buff) ? RES_OK : RES_ERROR;
    }
    UINT n = ra_window < count ? count : ra_window;
    ra_window = ra_window * 2 > RA_MAX ? RA_MAX : ra_window * 2;
    if (!sd_read(sector, n, ra_buf)) {
        ra_count = 0;
        return sd_read(sector, count, buff) ? RES_OK : RES_ERROR;  // 末尾を越えたときなど
    }
    // 先読みした中に、まだ書いていないセクタがあれば、その中身にしておく。読むたびに重ねるだけでは足りない:
    // 書き出した後は「まだ書いていない」印が消えるので、先読みに残った古い中身を返してしまう
    // （ビルドで書いたばかりの .o を読み直すときに起き、libc.a に古い中身が入った）
    for (UINT k = 0; k < n && wb_count; k++) {
        uint8_t *w = wb_lookup(sector + k);
        if (w)
            memcpy(ra_buf + k * SD_SECTOR_SIZE, w, SD_SECTOR_SIZE);
    }
    ra_start = sector;
    ra_count = n;
    memcpy(buff, ra_buf, count * SD_SECTOR_SIZE);
    return RES_OK;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || count == 0)
        return RES_PARERR;
    if (status & STA_NOINIT)
        return RES_NOTRDY;
    diskio_writeback(false);
    // 順に読んでいるか（どこから返すときも数える。数えないと先読みの量が戻らず、読みすぎた）
    bool sequential = sector == last_read_end;
    last_read_end = sector + count;
    if (!sequential)
        ra_window = RA_MIN;
    // 1 セクタで、まだ書いていなければ、それを返す
    uint8_t *w = count == 1 ? wb_lookup(sector) : nullptr;
    if (w) {
        memcpy(buff, w, SD_SECTOR_SIZE);
        return RES_OK;
    }
    DRESULT r = read_media(buff, sector, count, sequential);
    if (r != RES_OK || !wb_count)
        return r;
    // まだ書いていないセクタを、読んだものの上に重ねる（先読み・キャッシュの中身は古いことがある）
    for (UINT k = 0; k < count; k++) {
        w = wb_lookup(sector + k);
        if (w)
            memcpy(buff + k * SD_SECTOR_SIZE, w, SD_SECTOR_SIZE);
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || count == 0)
        return RES_PARERR;
    if (status & STA_NOINIT)
        return RES_NOTRDY;
    ra_invalidate(sector, count);
    for (UINT k = 0; k < count; k++) {
        uint8_t *c = sc_lookup(sector + k);
        if (c)
            memcpy(c, buff + k * SD_SECTOR_SIZE, SD_SECTOR_SIZE);
        if (!wb_put(sector + k, buff + k * SD_SECTOR_SIZE))
            return RES_ERROR;
    }
    diskio_writeback(false);
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0)
        return RES_PARERR;
    if (status & STA_NOINIT)
        return RES_NOTRDY;

    switch (cmd) {
    case CTRL_SYNC:
        // f_sync・f_close などから。すぐには書かず、期限が来たら書く（diskio_writeback）
        return diskio_writeback(false) ? RES_OK : RES_ERROR;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = SD_SECTOR_SIZE;
        return RES_OK;
    default:
        // GET_SECTOR_COUNT などは f_mkfs 用。f_mkfs は使わない（FF_USE_MKFS = 0）
        return RES_PARERR;
    }
}
