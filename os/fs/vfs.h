// VFS（ファイルディスクリプタの表）
// fd 0〜2 はコンソール（UART）、3 以降は FatFs のファイル。
// POSIX の open/read/write などは vfs.c で定義し、picolibc の stdio から使われる。
#pragma once

#include <stddef.h>
#include <stdint.h>

// fd の数（0〜2 はコンソール）。ファイル・ソケット・pipe で番号を分け合い、POSIX と同じく空いているいちばん小さい
// 番号を使う。256 は picolibc の FD_SETSIZE と同じ（それより大きい番号は select で扱えない）。2026-11-20 は 64
// （QuickJS の test_rw_handler が pipe を 100 個作るので 2026-11-21 に 256 に）。その前はファイルが 3〜15 だった
#define VFS_MAX_FILES   256
#define VFS_MAX_DIRS    32      // 同時に開いておけるフォルダ（4 では、sshfs の上の find（bfs）が 5 つ目から EPERM になった。2026-11-20）

// ソケットの fd を取る・返す（os/libc/socket.c。ファイルと同じ番号の並びから）。取れなければ -1
int vfs_fd_take_socket(void);
void vfs_fd_put_socket(int fd);
bool vfs_fd_open(int fd);   // 開いているファイルの fd か（select・poll）

// SD カードの FAT ボリュームをマウントする。成功すると true
bool vfs_init(void);

// owner が開いた fd 3 以降のファイルとディレクトリを閉じる（0 = アプリ。アプリの終了後に。
// 1〜 = サービス。サービスを止めたとき。os/kernel/svc.c）
void vfs_close_owner(int owner);
// fd を、アプリ・サービスの終わりに閉じないものにする（持ち主 VFS_OWNER_KERNEL。シェルのリダイレクト）
#define VFS_OWNER_KERNEL (-1000)     // アプリの持ち主（0、入れ子は -1、-2…）・サービス（1〜）と重ならない値
void vfs_keep_open(int fd);

// SD カードを直接読み書きする（FatFs と同じロックの中で）。成功なら 0
int vfs_raw_read(uint32_t sector, uint32_t count, void *buf);
int vfs_raw_write(uint32_t sector, uint32_t count, const void *buf);
// 遅らせている書き込みと読み出しのキャッシュを捨てる（SD カードを直接書き換えた後に。すぐ再起動すること）
void vfs_discard(void);
// ファイルの大きさと FAT の生の日時（fdate << 16 | ftime）。ふつうのファイルでなければ false。vfs_now_stamp は今の
// 日時を同じ形で（FatFs が書くときと同じ。時計が合う前は決まった値）
bool vfs_file_stamp(const char *path, uint64_t *size, uint32_t *stamp);
uint32_t vfs_now_stamp(void);
int vfs_exclusive(int (*fn)(void *), void *arg);   // SD カードを独り占めして fn を呼ぶ（書き出してから）

// ディレクトリの読み出し（dirent.c の opendir/readdir から使う）。
// FatFs と picolibc の DIR 型がぶつかるので、番号で受け渡す。
int vfs_dir_open(const char *path);
// 次の項目の名前を name に入れる。終わりなら 0、エラーなら -1、それ以外は 1
int vfs_dir_read(int handle, char *name, size_t size, bool *is_dir, bool *hidden);  // hidden: FAT の隠し属性
// readdir が最後に返したものが FAT の隠しファイルか（os/libc/dirent.c。アプリには monocs.h。引数は DIR *）
bool monocs_dirent_hidden(void *dir);
int monocs_fat_attr(const char *path, int set, int clear);  // FAT の属性を変える・読む（monocs.h）
int vfs_dir_close(int handle);

// 遅らせている SD カードへの書き込みを書き出す（os/fs/diskio.c のライトバック）。
// force でなければ、最初の書き込みから 1 秒たっているときだけ（シェルが入力を待つ間に繰り返し呼ぶ）
void vfs_sync(bool force);

// ファイルのキャッシュ（読み出し専用で開いたファイルをメモリに置く。vfs.c）の様子
struct vfs_cache_info {
    size_t bytes;       // 置いている量
    unsigned files;     // 置いているファイルの数
    uint64_t hits;      // キャッシュから開いた回数
    uint64_t misses;    // SD カードから読んだ回数
    unsigned missing;   // ないとわかっているパスの数（負のキャッシュ）
    uint64_t missing_hits; // 負のキャッシュで答えた回数
};
void vfs_cache_info(struct vfs_cache_info *info);

// ファイルの日時を変える（POSIX の utime。picolibc の utime.h は宣言しないので、ここに書く。times が NULL なら今）
struct utimbuf;
int utime(const char *path, const struct utimbuf *times);
