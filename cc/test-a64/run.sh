#!/bin/sh
# slimcc-a64 のテスト。cc/ で実行する: sh test-a64/run.sh
#   1〜5: フロントエンド（AArch64 向けの型・マクロ・構文解析）
#   6〜9: コード生成（test/ の実行、Cortex-A53 での実行、GCC との呼び出し規約の互換性、
#         自分自身のコンパイル）
#   10:   内蔵アセンブラ（GNU as とバイト単位で一致すること）
#   11:   内蔵アセンブラが書き出す ELF の .o（GNU as の .o と中身が一致すること）
#   12:   libslimcc（メモリ上でのコンパイル・リンク・実行。2 回ずつコンパイルして出力の一致も確かめる）
# 6〜9 は既定の内蔵アセンブラで行う（外部の as を使わない）
set -u

CC=./slimcc
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
status=0

ok()   { echo "  OK   $1"; }
ng()   { echo "  FAIL $1"; status=1; }

echo "1. 型のレイアウト（GCC が生成した layout.c）"
gcc -std=gnu23 -o "$TMP/gen_layout" test-a64/gen_layout.c && "$TMP/gen_layout" > "$TMP/layout.c"
if cmp -s "$TMP/layout.c" test-a64/layout.c; then ok "layout.c は最新"; else ng "layout.c が古い（gen_layout.c から作り直す）"; fi
gcc -std=gnu23 -fsyntax-only test-a64/layout.c && ok "gcc" || ng "gcc"
$CC -std=gnu23 -fsyntax-only test-a64/layout.c && ok "slimcc-a64" || ng "slimcc-a64"

echo "2. glibc（aarch64）のヘッダー"
for std in "" -std=c11 -std=c23; do
    $CC $std -fsyntax-only test-a64/headers.c && ok "headers.c ${std:-既定}" || ng "headers.c ${std:-既定}"
done

echo "3. 予定義マクロ"
$CC -dM -E - </dev/null > "$TMP/macros"
for m in __aarch64__ __ARM_ARCH __ARM_PCS_AAPCS64 __CHAR_UNSIGNED__; do
    grep -q "^#define $m " "$TMP/macros" && ok "$m" || ng "$m がない"
done
for m in __x86_64__ __amd64__; do
    grep -q "^#define $m " "$TMP/macros" && ng "$m がある" || ok "$m なし"
done
grep -q '^#define __WCHAR_TYPE__ unsigned int$' "$TMP/macros" && ok "__WCHAR_TYPE__" || ng "__WCHAR_TYPE__"

echo "4. test/*.c の構文解析（-fsyntax-only）"
expected=$(awk '$2 == "syntax" { print $1 }' test-a64/x86_assumptions.txt)
expected_all=$(awk '/^test\// { print $1 }' test-a64/x86_assumptions.txt)
pass=0
for f in test/*.c; do
    if $CC -fsyntax-only -Itest/ --tmpflagstdgnu2y "$f" >"$TMP/out" 2>&1; then
        pass=$((pass + 1))
        echo "$expected" | grep -qx "$f" && ng "$f が通るようになった（x86_assumptions.txt から外す）"
    elif echo "$expected" | grep -qx "$f"; then
        :
    else
        ng "$f"
        head -4 "$TMP/out" | sed 's/^/       /'
    fi
done
ok "$pass 本が通過（x86 前提の $(echo "$expected" | grep -c .) 本を除く）"

echo "5. slimcc-a64 自身のソース"
for f in alloc.c bitint.c codegen.c hashmap.c main.c parse.c platform.c preprocess.c strings.c tokenize.c type.c unicode.c; do
    $CC -fsyntax-only "$f" || ng "$f"
done
ok "自身のソースを解析できる"

echo "6. test/*.c をコンパイルして実行"
gcc -o "$TMP/common.o" -c test/host/common.c
pass=0
for f in test/*.c; do
    b=$(basename "$f" .c)
    if $CC "$f" -Itest/ --tmpflagstdgnu2y -pthread "$TMP/common.o" -o "$TMP/$b.exe" >"$TMP/out" 2>&1 &&
       timeout 30 "$TMP/$b.exe" >"$TMP/out" 2>&1; then
        pass=$((pass + 1))
        echo "$expected_all" | grep -qx "$f" && ng "$f が通るようになった（x86_assumptions.txt から外す）"
    elif echo "$expected_all" | grep -qx "$f"; then
        rm -f "$TMP/$b.exe"
    else
        ng "$f"
        tail -2 "$TMP/out" | cut -c1-200 | sed 's/^/       /'
        rm -f "$TMP/$b.exe"
    fi
done
ok "$pass 本が通過（x86 前提の $(echo "$expected_all" | grep -c .) 本を除く）"

echo "7. Cortex-A53（qemu-aarch64 -cpu cortex-a53）での実行と、LSE 命令がないこと"
if command -v qemu-aarch64 >/dev/null; then
    n=0
    for exe in "$TMP"/*.exe; do
        timeout 120 qemu-aarch64 -cpu cortex-a53 -L / "$exe" >/dev/null 2>&1 || ng "qemu: $(basename "$exe")"
        n=$((n + 1))
    done
    ok "$n 本を Cortex-A53 で実行"
else
    ng "qemu-aarch64 がない（sudo apt install qemu-user）"
fi
lse=0
for f in test/*.c; do
    $CC -S -Itest/ --tmpflagstdgnu2y -o "$TMP/out.s" "$f" 2>/dev/null || continue
    if grep -qE '^\s*(ldadd|ldclr|ldeor|ldset|ldsmax|ldsmin|ldumax|ldumin|swp|cas|casp)[albh]*\s' "$TMP/out.s"; then
        ng "$f に LSE 命令がある"
        lse=1
    fi
done
[ $lse -eq 0 ] && ok "LSE 命令なし"

echo "8. GCC との呼び出し規約の互換性（test-a64/abi_gen.py、両方向）"
for seed in 1 2 3; do
    python3 test-a64/abi_gen.py "$TMP" $seed 300
    for pair in "$CC gcc" "gcc $CC"; do
        set -- $pair
        if $1 -c -o "$TMP/caller.o" "$TMP/caller.c" && $2 -c -o "$TMP/callee.o" "$TMP/callee.c" &&
           gcc -o "$TMP/abi" "$TMP/caller.o" "$TMP/callee.o" && "$TMP/abi" >"$TMP/out"; then
            ok "種 $seed、呼び出し側 $(basename $1)、呼ばれる側 $(basename $2): $(tail -1 "$TMP/out")"
        else
            ng "種 $seed、呼び出し側 $(basename $1)、呼ばれる側 $(basename $2)"
            tail -5 "$TMP/out" | sed 's/^/       /'
        fi
    done
done

echo "8b. ループの変形（反転・展開・LICM・命令の並べ替え。test-a64/loop_gen.py）が GCC と同じ結果"
for seed in 1 2 3 4; do
    python3 test-a64/loop_gen.py "$TMP/loop.c" $seed 300
    gcc -O1 -w -o "$TMP/loop_gcc" "$TMP/loop.c" && "$TMP/loop_gcc" >"$TMP/loop_gcc.out"
    for fl in "" "-fno-unroll-loops -fno-schedule-insns2"; do
        if $CC $fl -o "$TMP/loop_slim" "$TMP/loop.c" && "$TMP/loop_slim" >"$TMP/loop_slim.out" &&
           cmp -s "$TMP/loop_gcc.out" "$TMP/loop_slim.out"; then
            ok "種 $seed ${fl:-（既定）}: $(wc -l < "$TMP/loop_gcc.out") 行"
        else
            ng "種 $seed ${fl:-（既定）}"
            diff "$TMP/loop_gcc.out" "$TMP/loop_slim.out" | head -5 | sed 's/^/       /'
        fi
    done
done

echo "8d. switch の振り分け（case が多いときの二分探索。test-a64/switch_gen.py）が GCC と同じ結果"
for seed in 1 2 3 4; do
    python3 test-a64/switch_gen.py "$TMP/sw.c" $seed 300
    if gcc -w -o "$TMP/sw_gcc" "$TMP/sw.c" && "$TMP/sw_gcc" >"$TMP/sw_gcc.out" &&
       $CC -w -o "$TMP/sw_slim" "$TMP/sw.c" && "$TMP/sw_slim" >"$TMP/sw_slim.out" &&
       cmp -s "$TMP/sw_gcc.out" "$TMP/sw_slim.out"; then
        ok "種 $seed: $(wc -l < "$TMP/sw_gcc.out") 行"
    else
        ng "種 $seed"
        diff "$TMP/sw_gcc.out" "$TMP/sw_slim.out" | head -5 | sed 's/^/       /'
    fi
done

echo "8c. GCC と同じ結果になること（test-a64/gccmatch/*.c: va_arg、あふれの確かめなど）"
for f in test-a64/gccmatch/*.c; do
    n=$(basename "$f" .c)
    if gcc -w -o "$TMP/gm_gcc" "$f" && "$TMP/gm_gcc" >"$TMP/gm_gcc.out" &&
       $CC -o "$TMP/gm_slim" "$f" && "$TMP/gm_slim" >"$TMP/gm_slim.out" && cmp -s "$TMP/gm_gcc.out" "$TMP/gm_slim.out"; then
        ok "$n: $(wc -l < "$TMP/gm_gcc.out") 行"
    else
        ng "$n"
        diff "$TMP/gm_gcc.out" "$TMP/gm_slim.out" | head -5 | sed 's/^/       /'
    fi
done

echo "9. 自分自身のコンパイル（2 世代目と 3 世代目の一致）"
srcs="alloc assembler bitint codegen hashmap main options parse platform preprocess strings tokenize type unicode"
mkdir -p "$TMP/s2" "$TMP/s3"
for f in $srcs; do $CC -c -o "$TMP/s2/$f.o" "$f.c" || ng "2 世代目: $f.c"; done
(cd "$TMP/s2" && "$OLDPWD/$CC" -o slimcc *.o) || ng "2 世代目のリンク"
ln -sf "$PWD/slimcc_headers" "$TMP/s2/slimcc_headers"
for f in $srcs; do "$TMP/s2/slimcc" -c -o "$TMP/s3/$f.o" "$f.c" || ng "3 世代目: $f.c"; done
(cd "$TMP/s3" && "$TMP/s2/slimcc" -o slimcc *.o) || ng "3 世代目のリンク"
if cmp -s "$TMP/s2/slimcc" "$TMP/s3/slimcc"; then ok "2 世代目と 3 世代目が一致"; else ng "2 世代目と 3 世代目が異なる"; fi

echo "10. 内蔵アセンブラと GNU as の比較（セクション、リロケーション、シンボル）"
mkdir -p "$TMP/asm"
for f in test/*.c; do
    $CC -S -Itest/ --tmpflagstdgnu2y -o "$TMP/asm/t_$(basename "$f" .c).s" "$f" 2>/dev/null
done
for f in $srcs; do
    $CC -S -o "$TMP/asm/cc_$f.s" "$f.c"
done
for seed in 1 2 3; do
    python3 test-a64/asm_gen.py "$TMP/asm/gen$seed.s" $seed
done
# 手書きのテスト（.macro、リテラルプール、SYS 命令）と、Monocs の .S（前処理したもの）
for f in test-a64/asm/*.asm; do
    cp "$f" "$TMP/asm/hand_$(basename "$f" .asm).s"
done
for f in ../os/boot/boot.S ../os/kernel/vectors.S; do
    gcc -E -P -x assembler-with-cpp -o "$TMP/asm/os_$(basename "$f" .S).s" "$f"
done
if sh test-a64/asm_compare.sh "$TMP/asm" "$TMP"/asm/*.s >"$TMP/out" 2>&1; then
    ok "$(tail -1 "$TMP/out")"
else
    ng "内蔵アセンブラ"
    tail -20 "$TMP/out" | sed 's/^/       /'
fi

echo "11. ELF の .o（内蔵アセンブラと GNU as の .o を test-a64/objdump.py で比べる）"
mkdir -p "$TMP/elf"
same=0
for f in test/*.c $(for s in $srcs; do echo "$s.c"; done); do
    b=$(echo "$f" | tr / _)
    if ! $CC -c -Itest/ --tmpflagstdgnu2y -o "$TMP/elf/$b.ia.o" "$f" 2>"$TMP/out" ||
       ! $CC -fno-integrated-as -c -Itest/ --tmpflagstdgnu2y -o "$TMP/elf/$b.gas.o" "$f" 2>>"$TMP/out"; then
        ng "$f のコンパイル"
        continue
    fi
    python3 test-a64/objdump.py "$TMP/elf/$b.ia.o" > "$TMP/elf/ia.txt"
    python3 test-a64/objdump.py "$TMP/elf/$b.gas.o" > "$TMP/elf/gas.txt"
    if cmp -s "$TMP/elf/ia.txt" "$TMP/elf/gas.txt"; then
        same=$((same + 1))
    else
        ng "$f の .o が一致しない"
    fi
    if readelf -a "$TMP/elf/$b.ia.o" 2>&1 >/dev/null | grep -qi 'warn\|error'; then
        ng "$f の .o に readelf が警告を出した"
    fi
done
ok "$same 本の .o が一致"

echo "12. libslimcc（slimcc-jit で test/*.c をメモリ上でコンパイル・リンク・実行）"
make -s slimcc-jit CFLAGS=-O2 >/dev/null 2>&1 || ng "slimcc-jit のビルド"
unsupported=$(awk '/^test\// { print $1 }' test-a64/jit_unsupported.txt)
pass=0
for f in test/*.c; do
    echo "$unsupported" | grep -qx "$f" && continue
    # test/macro.c は __TIME__ を使うので、2 回のコンパイルが秒の変わり目をまたぐと一致しない（1 回だけにする）
    rep=2
    [ "$f" = test/macro.c ] && rep=1
    if timeout 120 ./slimcc-jit -Itest --tmpflagstdgnu2y -repeat $rep "$f" >"$TMP/out" 2>&1; then
        pass=$((pass + 1))
    else
        ng "$f"
        tail -2 "$TMP/out" | sed 's/^/       /'
    fi
done
ok "$pass 本が通過（対象外の $(echo "$unsupported" | grep -c .) 本を除く）"
r=$(./slimcc-jit -Itest --tmpflagstdgnu2y -repeat 100 test/function.c 2>&1 >/dev/null | grep 'compile #' | sed 's/.*max RSS \([0-9]*\) KB/\1/' | tr '\n' ' ')
set -- $r
if [ $# -eq 2 ] && [ $(( $2 - $1 )) -lt 4096 ]; then
    ok "100 回コンパイルしても、メモリ使用量がほぼ増えない（$1 KB → $2 KB）"
else
    ng "100 回のコンパイルでメモリ使用量が増えた（$r）"
fi

pass=0
for f in test/*.c; do
    echo "$unsupported" | grep -qx "$f" && continue
    if timeout 120 ./slimcc-jit -Itest --tmpflagstdgnu2y -roundtrip "$f" >"$TMP/out" 2>&1; then
        pass=$((pass + 1))
    else
        ng "$f（書いて読み戻したもの）"
        tail -2 "$TMP/out" | sed 's/^/       /'
    fi
done
ok "コンパイルした結果を書いて読み戻したもの（slimcc_obj_save・load。Monocs のキャッシュ）でも $pass 本が通過"
gcc -o "$TMP/link_gcc" test-a64/link/a.c test-a64/link/b.c && "$TMP/link_gcc" x > "$TMP/link_gcc.out"
if ./slimcc-jit test-a64/link/a.c test-a64/link/b.c -- x > "$TMP/link_jit.out" 2>&1 &&
   cmp -s "$TMP/link_gcc.out" "$TMP/link_jit.out"; then
    ok "複数のオブジェクトのリンク（static・弱い定義・ほかのファイルの関数と変数）が GCC と同じ"
else
    ng "複数のオブジェクトのリンク"
    diff "$TMP/link_gcc.out" "$TMP/link_jit.out" | head -5 | sed 's/^/       /'
fi
if ./slimcc-jit test-a64/link/a.c test-a64/link/b.c test-a64/link/dup.c 2>&1 | grep -q "multiple definition of 'counter'"; then
    ok "強い定義が 2 つあればリンクのエラー"
else
    ng "強い定義が 2 つあってもエラーにならない"
fi

echo "13. 実行時ライブラリ（rt/softfp128.c を libgcc と比べる。結果と例外フラグ、4 つの丸めモード）"
if gcc -O2 -DRT_TEST -DTEST_ALL -o "$TMP/test_softfp" rt/test_softfp.c rt/softfp128.c &&
   r=$("$TMP/test_softfp" 1000000 | tail -1) && echo "$r" | grep -q ' 0 failures'; then
    ok "GCC でコンパイルした版: $r"
else
    ng "GCC でコンパイルした版: $r"
fi
if ./slimcc -DRT_TEST -c -o "$TMP/softfp_slim.o" rt/softfp128.c &&
   gcc -O2 -DRT_TEST -DTEST_ALL -o "$TMP/test_softfp_slim" rt/test_softfp.c "$TMP/softfp_slim.o" &&
   r=$("$TMP/test_softfp_slim" 300000 | tail -1) && echo "$r" | grep -q ' 0 failures'; then
    ok "slimcc でコンパイルした版: $r"
else
    ng "slimcc でコンパイルした版: $r"
fi

echo "14. long double と 10 進の変換（os/libc/ldtoa.c を glibc と比べる。printf の桁、strtold）"
if gcc -O2 -DLDTOA_TEST -o "$TMP/test_ldtoa" ../tools/picolibc/test_ldtoa.c ../os/libc/ldtoa.c &&
   r=$("$TMP/test_ldtoa" 3000 | tail -1) && echo "$r" | grep -q ' 0 failures'; then
    ok "GCC でコンパイルした版: $r"
else
    ng "GCC でコンパイルした版: $r"
fi
if ./slimcc -DLDTOA_TEST -c -o "$TMP/ldtoa_slim.o" ../os/libc/ldtoa.c &&
   gcc -O2 -o "$TMP/test_ldtoa_slim" ../tools/picolibc/test_ldtoa.c "$TMP/ldtoa_slim.o" &&
   r=$("$TMP/test_ldtoa_slim" 1000 | tail -1) && echo "$r" | grep -q ' 0 failures'; then
    ok "slimcc でコンパイルした版: $r"
else
    ng "slimcc でコンパイルした版: $r"
fi

[ $status -eq 0 ] && echo "すべて成功" || echo "失敗あり"
exit $status
