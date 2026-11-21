// シェル
#pragma once

[[noreturn]] void shell_main(void);

// 1 つのコマンドを動かす（組み込みのコマンド、PATH のアプリ）。終了状態
int run_command(int argc, char **argv);

// POSIX の system()（OS API 表）。アプリの中からコマンドの行を動かす（入れ子）
int shell_system(const char *cmd);
// シェルが最後に動かした行の終了の値（$? と同じ。httpd の /.status、nos.py cmd の終了の値）
int shell_last_status(void);
int shell_system_redirect(const char *cmd, const char *in_path, const char *out_path);  // popen 用

// ウォッチドッグで再起動する。sync なら、遅らせている SD カードへの書き込みを先に済ませる
// （sync でないのは、SD カードを直接書き換えた後。メモリ上の古い FAT などを書き戻さないように）
void system_reboot(bool sync);
