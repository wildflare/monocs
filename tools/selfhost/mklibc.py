#!/usr/bin/env python3
# セルフホスト用に、picolibc を Monocs 上でビルドするための材料を集める（make sdimage-self から呼ばれる）。
#   python3 tools/selfhost/mklibc.py 出力ディレクトリ（build/selfsrc）
#
# tools/picolibc/build.py（Linux 上で slimcc で picolibc をビルドする）と同じコマンドを、
# Monocs 上のビルドスクリプト（tools/selfhost/buildlibc.c）が使う形（libc_files.h）にする。
# ソースと、それが読むヘッダー（slimcc -M で調べる）を、リポジトリと同じ相対パスで出力ディレクトリに写す。
# コンパイルは build/picolibc で行い、パスもそこからの相対パス（Linux 上のビルドと同じ。__FILE__ などが一致するように）。
import os
import shlex
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.dont_write_bytecode = True  # tools/picolibc に __pycache__ を作らない
sys.path.insert(0, os.path.join(ROOT, "tools/picolibc"))
import build as pb  # noqa: E402  tools/picolibc/build.py

CWD = os.path.join(ROOT, "build/picolibc")


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    out_dir = sys.argv[1]
    tasks, members = pb.plan()
    flagsets = []
    files = set()
    entries = []
    for out, obj, argv, cwd in tasks:
        assert cwd == CWD, cwd
        # argv: [CC, オプション..., 入力, "-o", obj]。Monocs の slimcc には CC と -o の後を除いて渡す
        opts = [a for a in argv[1:-2] if a != "-c"]
        src = opts[-1]
        opts = opts[:-1]
        key = tuple(opts)
        if key not in flagsets:
            flagsets.append(key)
        # 読むヘッダー
        r = subprocess.run([argv[0]] + list(opts) + ["-M", src], cwd=CWD, capture_output=True,
                           text=True)
        if r.returncode:
            sys.exit("mklibc: %s: %s" % (src, r.stderr))
        deps = r.stdout.replace("\\\n", " ").split(":", 1)[1].split()
        for d in deps + [src]:
            files.add(os.path.relpath(os.path.normpath(os.path.join(CWD, d)), ROOT))
        entries.append((src, os.path.relpath(obj, CWD), flagsets.index(key)))

    # slimcc がシステムのヘッダーのディレクトリから自動で読むもの（#include ではないので -M に出ない）
    files.add("cc/slimcc_headers/include/bitint_builtins")
    for f in sorted(files):
        if f.startswith(".."):
            sys.exit("mklibc: outside of the repository: " + f)
        dst = os.path.join(out_dir, f)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(os.path.join(ROOT, f), dst)
    # build/picolibc（コンパイルするディレクトリ）がなければ作っておく
    os.makedirs(os.path.join(out_dir, "build/picolibc"), exist_ok=True)

    with open(os.path.join(out_dir, "libc_files.h"), "w") as h:
        h.write("// tools/selfhost/mklibc.py が作る。build/picolibc からの相対パス\n")
        h.write("#define LIBC_NFLAGS %d\n" % len(flagsets))
        h.write("static const char *const libc_flags[][%d] = {\n" % (max(len(f) for f in flagsets) + 1))
        for f in flagsets:
            h.write("    {" + ", ".join(c_str(a) for a in f) + ", 0},\n")
        h.write("};\n")
        h.write("static const struct { const char *src, *obj; int flags; } libc_files[] = {\n")
        for src, obj, fl in entries:
            h.write("    {%s, %s, %d},\n" % (c_str(src), c_str(obj), fl))
        h.write("};\n")
        # libc.a のメンバー（この順に並べる）。libc_files の添字
        index = {os.path.relpath(obj, CWD): i for i, (_, obj, _, _) in enumerate(tasks)}
        h.write("static const int libc_members[] = {\n")
        for m in members:
            h.write("    %d,\n" % index[os.path.relpath(m, CWD)])
        h.write("};\n")
    shutil.copyfile(os.path.join(ROOT, "tools/selfhost/buildlibc.c"),
                    os.path.join(out_dir, "buildlibc.c"))
    shutil.copyfile(os.path.join(ROOT, "tools/selfhost/ar.h"), os.path.join(out_dir, "ar.h"))
    print("mklibc: %d ファイルをコンパイル、%d ファイル（ソースとヘッダー）、オプション %d 通り"
          % (len(entries), len(files), len(flagsets)))


if __name__ == "__main__":
    main()
