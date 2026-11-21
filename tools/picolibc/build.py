#!/usr/bin/env python3
# picolibc を slimcc-a64 でビルドする（Linux 上のクロスビルド）。
#
# meson が GCC で設定したビルド（build/picolibc）の compile_commands.json を読み、
# コンパイラを cc/slimcc-monocs に置き換えて、同じソースを同じ設定（picolibc.h など）でコンパイルする。
# 結果は build/picolibc-slimcc/ に置き、GCC 版の libc.a と同じメンバー・同じ順で libc.a を作る。
# ただし複素数の関数（libm/complex）は除く（slimcc が _Complex に対応していないため）。
#
#   python3 tools/picolibc/build.py [-j N] [-k]
#     -k: 失敗しても続ける（最後に失敗したファイルの一覧を出す）
import json
import os
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MESON = os.path.join(ROOT, "build/picolibc")
OUT = os.path.join(ROOT, "build/picolibc-slimcc")
CC = os.path.join(ROOT, "cc/slimcc-monocs")
GCC_LIB = os.path.join(ROOT, "build/picolibc-install/lib/libc.a")

# 対象外のソース（slimcc が _Complex に対応していない）
EXCLUDE = ("/libm/complex/",)

# picolibc のアセンブリ版（memcpy.S など。NEON を使う）を選ばせるためのマクロ。
# GCC は Cortex-A53 で定義するが、slimcc は NEON の組み込み関数（arm_neon.h）に対応していないので、
# アプリには定義せず、picolibc のビルドにだけ渡す
EXTRA = ["-D__ARM_NEON=1", "-D__ARM_FEATURE_UNALIGNED=1"]

# slimcc に渡すオプション（そのほかの警告・最適化・コード生成の指定は使わない）
KEEP_PREFIX = ("-I", "-D", "-U", "-std=")
KEEP = {"-ffunction-sections", "-c"}


def convert(cmd):
    args = shlex.split(cmd["command"])
    out = None
    # slimcc のヘッダーは、コンパイルするディレクトリ（build/picolibc）からの相対パスで渡す
    # （Monocs 上のビルド（tools/selfhost/buildlibc.c）と同じパスにして、結果を一致させるため）
    sysinc = os.path.relpath(os.path.join(ROOT, "cc/slimcc_headers/include"), cmd["directory"])
    new = [CC, "-nostdinc", "-isystem", sysinc] + EXTRA
    i = 1
    while i < len(args):
        a = args[i]
        if a in ("-o", "-MF", "-MQ"):
            if a == "-o":
                out = args[i + 1]
            i += 2
            continue
        if a.startswith(KEEP_PREFIX) or a in KEEP:
            if a in ("-I", "-D", "-U"):
                new += [a, args[i + 1]]
                i += 2
                continue
            new.append(a)
        elif not a.startswith("-"):
            new.append(a)  # 入力ファイル
        i += 1
    obj = os.path.join(OUT, out)
    new += ["-o", obj]
    return out, obj, new


def plan():
    # コンパイルするもの [(meson の出力名, .o, コマンド, ディレクトリ)] と、libc.a のメンバーの .o（並び順）
    cmds = json.load(open(os.path.join(MESON, "compile_commands.json")))
    tasks = []
    for c in cmds:
        if any(x in c["file"] for x in EXCLUDE):
            continue
        out, obj, argv = convert(c)
        tasks.append((out, obj, argv, c["directory"]))
    # GCC 版の libc.a と同じメンバーを同じ順に並べる
    members = subprocess.run(["ar", "t", GCC_LIB], capture_output=True, text=True,
                             check=True).stdout.split()
    by_name = {}
    for out, obj, _, _ in tasks:
        by_name.setdefault(os.path.basename(out), []).append(obj)
    objs = []
    excluded = {os.path.basename(shlex.split(c["command"])[shlex.split(c["command"]).index("-o") + 1])
                for c in cmds if any(x in c["file"] for x in EXCLUDE)}
    for m in members:
        if m in excluded:
            continue
        cand = by_name.get(m)
        if not cand or len(cand) != 1:
            sys.exit(f"cannot map archive member {m}: {cand}")
        objs.append(cand[0])
    return tasks, objs


def main():
    jobs = os.cpu_count() or 4
    keep_going = "-k" in sys.argv
    if "-j" in sys.argv:
        jobs = int(sys.argv[sys.argv.index("-j") + 1])
    tasks, objs = plan()
    for _, obj, _, _ in tasks:
        os.makedirs(os.path.dirname(obj), exist_ok=True)

    def run(t):
        out, obj, argv, cwd = t
        r = subprocess.run(argv, cwd=cwd, capture_output=True, text=True)
        return out, r.returncode, r.stderr

    failed = []
    with ThreadPoolExecutor(jobs) as ex:
        for out, rc, err in ex.map(run, tasks):
            if rc:
                failed.append((out, err))
                if not keep_going:
                    print(err, file=sys.stderr)
                    sys.exit(f"failed: {out}")
    for out, err in failed:
        first = err.strip().splitlines()
        print(f"FAIL {out}: {' | '.join(first[:3])[:300]}")
    print(f"{len(tasks) - len(failed)} / {len(tasks)} compiled")
    if failed:
        sys.exit(1)

    lib = os.path.join(OUT, "libc.a")
    if os.path.exists(lib):
        os.remove(lib)
    subprocess.run(["ar", "rcs", lib] + objs, check=True)
    print(f"{lib}: {len(objs)} members")


if __name__ == "__main__":
    main()
