// opendir/readdir/closedir（picolibc の DIR 型の上に、vfs_dir_* をかぶせる）
// DIR.fd に vfs のディレクトリ番号を入れる。
// Linux と同じく、最初に「.」と「..」を返す（FAT のサブフォルダの . と .. は FatFs が飛ばし、ルートにはない。
// Linux のプログラムは、親へ移る項目として .. があることを前提にすることがある。2026-11-14）。
// DIR.offset に、返した「.」「..」の数を持つ（picolibc の readdir 用で、ここでは使わない）

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "../fs/vfs.h"

DIR *opendir(const char *path)
{
    DIR *d = malloc(sizeof(*d));
    if (!d) {
        errno = ENOMEM;
        return nullptr;
    }
    d->fd = vfs_dir_open(path);
    if (d->fd < 0) {
        free(d);
        return nullptr;
    }
    d->offset = 0;
    return d;
}

struct dirent *readdir(DIR *d)
{
    if (d->offset < 2) {
        snprintf(d->dirent.d_name, sizeof(d->dirent.d_name), "%s", d->offset == 0 ? "." : "..");
        d->offset++;
        d->buf[0] = false;
        d->dirent.d_ino = 0;
        d->dirent.d_type = DT_DIR;
        return &d->dirent;
    }
    bool is_dir, hidden;
    if (vfs_dir_read(d->fd, d->dirent.d_name, sizeof(d->dirent.d_name), &is_dir, &hidden) <= 0)
        return nullptr;
    d->buf[0] = hidden;     // monocs_dirent_hidden（DIR の buf は picolibc の readdir 用で、ここでは使わない）
    d->dirent.d_ino = 0;
    d->dirent.d_type = is_dir ? DT_DIR : DT_REG;
    return &d->dirent;
}

// 最後に readdir が返したものに、FAT の隠し属性が付いているか（ls が「.」で始まる名前と同じく隠す。
// Linux の vfat は隠し属性を見ないが、Monocs は Windows と同じく隠す。System Volume Information など）
bool monocs_dirent_hidden(void *dir)
{
    DIR *d = dir;
    return d && d->buf[0];
}

int closedir(DIR *d)
{
    int r = vfs_dir_close(d->fd);
    free(d);
    return r;
}
