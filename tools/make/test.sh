#!/bin/sh
# Monocs の Makefile の解釈器（os/slimcc/makefile.c）を、GNU make と比べる（Linux で）。
#   sh tools/make/test.sh
# 1. 式の展開: Makefile の後ろに $(info 式) を足して GNU make に展開させ、make_eval（makefile.c をそのまま
#    コンパイルしたもの）が同じ式を展開した結果と比べる（t1.mk と、ユーザーのプロジェクトの Makefile）
# 2. MakeInfo（プログラムの名前、オプション、.c の一覧）: t1 は t1.info と比べ、ユーザーのプロジェクトは表示する
# 3. 壊れた Makefile（乱数で壊した 2,000 通り）で落ちない
set -e
cd "$(dirname "$0")"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
gcc -std=c23 -g -O1 -Wall -fsanitize=address,undefined -o "$T/make_eval" make_eval.c
fail=0

# compare DIR MAKEFILE EXPRS
compare() {
    dir=$1 mk=$2 exprs=$3
    cp "$mk" "$T/gnu.mk"
    printf '\n' >> "$T/gnu.mk"
    while IFS= read -r e; do printf '$(info %s)\n' "$e" >> "$T/gnu.mk"; done < "$exprs"
    printf '__monocs_none: ;\n' >> "$T/gnu.mk"
    (cd "$dir" && make -s -f "$T/gnu.mk" __monocs_none) > "$T/gnu.out"
    set --
    while IFS= read -r e; do set -- "$@" "$e"; done < "$exprs"
    "$T/make_eval" "$dir" "$mk" "$@" > "$T/monocs.out"
    if diff "$T/gnu.out" "$T/monocs.out" > "$T/diff"; then
        echo "ok: $mk ($(wc -l < "$exprs") expressions)"
    else
        echo "DIFF: $mk (< GNU make, > Monocs)"; cat "$T/diff"; fail=1
    fi
}

mkdir -p "$T/t1/sub"
touch "$T/t1/a.c" "$T/t1/b.c" "$T/t1/zz.c" "$T/t1/.hidden.c" "$T/t1/sub/c.c"
cp t1.mk "$T/t1/Makefile"
compare "$T/t1" "$T/t1/Makefile" t1.exprs
# プログラムの決め方（all: $(OBJS) の .o から .c、オプションの取り出し）
"$T/make_eval" -info "$T/t1" | sed "s|$T/t1|DIR|" > "$T/t1.info"
if diff t1.info "$T/t1.info"; then echo "ok: t1 MakeInfo"; else echo "DIFF: t1 MakeInfo"; fail=1; fi
# t2: Lua の形（既定のゴールが make を呼び直す → all、.a のライブラリを開く）
mkdir -p "$T/t2"
touch "$T/t2/prog.c" "$T/t2/tool.c" "$T/t2/core1.c" "$T/t2/core2.c" "$T/t2/lib1.c"
cp t2.mk "$T/t2/Makefile"
"$T/make_eval" -info "$T/t2" | sed "s|$T/t2|DIR|" > "$T/t2.info"
if diff t2.info "$T/t2.info"; then echo "ok: t2 MakeInfo (Lua の形)"; else echo "DIFF: t2 MakeInfo"; fail=1; fi
# t3: QuickJS の形（レシピのない all、.PHONY、$(shell cat VERSION)、order-only の前提、サブフォルダの .o）
mkdir -p "$T/t3/libm"
touch "$T/t3/qjs.c" "$T/t3/core.c" "$T/t3/libm/util.c"
echo 2026-06-04 > "$T/t3/VERSION"
cp t3.mk "$T/t3/Makefile"
"$T/make_eval" -info "$T/t3" | sed "s|$T/t3|DIR|" > "$T/t3.info"
if diff t3.info "$T/t3.info"; then echo "ok: t3 MakeInfo (QuickJS の形)"; else echo "DIFF: t3 MakeInfo"; fail=1; fi

# make の引数（シェルの make が書く .monocs-make）と、コマンドの中の $(MAKE) の呼び直し
mk_check() {    # 名前 フォルダ .monocs-make の中身 期待する target と flag（1 行に）
    if [ -n "$3" ]; then echo "$3" > "$2/.monocs-make"; else rm -f "$2/.monocs-make"; fi
    got=$("$T/make_eval" -info "$2" | grep -v '^src' | tr '\n' ' ')
    if [ "$got" = "$4" ]; then echo "ok: $1"; else echo "DIFF: $1: got [$got] want [$4]"; fail=1; fi
}
mk_check "make posix" "$T/t2" posix "target: prog flag: -DCOMPAT_X flag: -DUSE_POSIX "
mk_check "make linux (前提の目標のコマンド)" "$T/t2" linux "target: prog flag: -DCOMPAT_X flag: -DUSE_LINUX "
mk_check "make posix MYCFLAGS=-DX=1" "$T/t2" "posix 'MYCFLAGS=-DX=1'" "target: prog flag: -DCOMPAT_X flag: -DUSE_POSIX flag: -DX=1 "
mk_check "make nosuch（見つからない）" "$T/t2" nosuch "target:  flag: -DCOMPAT_X "
rm -f "$T/t2/.monocs-make"
mkdir -p "$T/t2top/sub"
cp t2top.mk "$T/t2top/Makefile"
cp t2.mk "$T/t2top/sub/Makefile"
touch "$T/t2top/sub/prog.c" "$T/t2top/sub/core1.c" "$T/t2top/sub/core2.c" "$T/t2top/sub/lib1.c"
mk_check "上のフォルダ、make（guess → Monocs はない → all）" "$T/t2top" "" "target: prog flag: -DCOMPAT_X "
mk_check "上のフォルダ、make posix（cd sub && make posix）" "$T/t2top" posix "target: prog flag: -DCOMPAT_X flag: -DUSE_POSIX "

# ユーザーのプロジェクト（あれば）
for p in "$HOME/dev/tools/file-dir-manager" "$HOME/dev/tools/mini-editor" "$HOME/dev/tools/gt"; do
    [ -f "$p/Makefile" ] || continue
    printf '%s\n' '$(CFLAGS) $(CPPFLAGS)' '$(LDLIBS) $(LIBS)' '$(TARGET)' '$(SRC) $(SRCS)' '$(OBJS)' > "$T/p.exprs"
    compare "$p" "$p/Makefile" "$T/p.exprs"
    "$T/make_eval" -info "$p" | sed 's/^/    /' | head -8
done

# 3. 壊れた Makefile でも落ちない（乱数で文字を入れ替え・消し・足したもの。検査付きでコンパイルしてある）
python3 - "$T" t1.mk <<'PY' || fail=1
import os, random, subprocess, sys
t, src = sys.argv[1], open(sys.argv[2], 'rb').read()
d = os.path.join(t, 'fuzz'); os.makedirs(d, exist_ok=True)
rnd = random.Random(1)
junk = b'$(){}:=+?!#\\\n\t ,%ifeqendefine'
for i in range(2000):
    b = bytearray(src)
    for _ in range(rnd.randint(1, 20)):
        k = rnd.randrange(len(b) + 1); op = rnd.randrange(3)
        if op == 0 and k < len(b): del b[k]
        elif op == 1 and k < len(b): b[k] = rnd.choice(junk)
        else: b[k:k] = bytes([rnd.choice(junk)])
    open(os.path.join(d, 'Makefile'), 'wb').write(b)
    r = subprocess.run([os.path.join(t, 'make_eval'), '-info', d], capture_output=True, timeout=10)
    if r.returncode != 0:
        open(os.path.join(t, 'crash.mk'), 'wb').write(b)
        print('CRASH (fuzz %d)' % i); print(r.stderr.decode()[-2000:]); sys.exit(1)
print('ok: 2000 broken Makefiles')
PY
exit $fail
