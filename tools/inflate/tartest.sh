#!/bin/sh
# /bin/tar・/bin/gunzip のテスト（Linux。GCC の検査付きでコンパイルして、GNU tar・gzip と比べる）:
#   sh tools/inflate/tartest.sh
set -u
cd "$(dirname "$0")/../.."
R=$(pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
CF="-std=gnu23 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Ilib/inflate"
gcc $CF -o "$T/tar" bin/tar/tar.c lib/inflate/inflate.c || exit 1
gcc $CF -o "$T/gunzip" bin/gunzip/gunzip.c lib/inflate/inflate.c || exit 1
gcc $CF -o "$T/zcat" bin/zcat/zcat.c lib/inflate/inflate.c || exit 1
cd "$T"
fail=0
ng() { fail=$((fail + 1)); echo "FAIL $*"; }
# 材料: リポジトリの一部と、長い名前・深いフォルダ・空のファイル・空のフォルダ
mkdir -p src/pkg && cp -r "$R/bin" "$R/lib" "$R/services" src/pkg/
long=$(printf 'd%.0s' $(seq 1 60)); mkdir -p "src/pkg/$long/$long/$long" src/pkg/emptydir
echo deep > "src/pkg/$long/$long/$long/file-with-a-rather-long-name-to-go-over-one-hundred-characters-in-total.txt"
: > src/pkg/empty.txt
head -c 1000000 /dev/urandom > src/pkg/random.bin
ln -s empty.txt src/pkg/link 2>/dev/null
for fmt in gnu ustar pax; do
    # ustar は長い名前を入れられないので、その分は警告になる（材料から外れるだけ）
    (cd src && tar --format=$fmt -cf ../a-$fmt.tar pkg 2>/dev/null); gzip -c a-$fmt.tar > a-$fmt.tar.gz
    for f in a-$fmt.tar a-$fmt.tar.gz; do
        mkdir want-$f got-$f
        tar -xf $f -C want-$f; ./tar xf $f -C got-$f 2> err-$f
        rm -f want-$f/pkg/link      # リンクは展開しない（飛ばして警告）
        diff -r want-$f got-$f > /dev/null || ng "extract $f"
        grep -q "links are not supported" err-$f || ng "link warning $f"
        tar -tf $f | sort > l1; ./tar tf $f | sort > l2; cmp -s l1 l2 || ng "list $f"
    done
done
mkdir s1 s2; tar -xf a-gnu.tar.gz -C s1 --strip-components=2 pkg/bin; ./tar xzf a-gnu.tar.gz -C s2 --strip-components=2 pkg/bin 2>/dev/null
diff -r s1 s2 > /dev/null && [ -f s2/ls.c ] || ng "strip-components and member"
mkdir p1; (cd p1 && ../tar xz < ../a-pax.tar.gz 2>/dev/null); rm -f want-a-pax.tar.gz/pkg/link; diff -r want-a-pax.tar.gz p1 > /dev/null || ng "stdin"
# 日時
[ "$(stat -c %Y got-a-gnu.tar/pkg/empty.txt)" = "$(stat -c %Y src/pkg/empty.txt)" ] || ng "mtime"
# 外に書かせない
python3 - <<'PY'
import tarfile, io
t = tarfile.open('evil.tar', 'w')
for name in ('../escape.txt', '/tmp/abs-escape-test.txt', 'ok/../../escape2.txt', 'fine.txt'):
    i = tarfile.TarInfo(name); d = b'x'; i.size = 1; t.addfile(i, io.BytesIO(d))
t.close()
PY
mkdir jail; ./tar xf evil.tar -C jail 2> evil.err
[ ! -e escape.txt ] && [ ! -e escape2.txt ] && [ -f jail/fine.txt ] && [ -f jail/tmp/abs-escape-test.txt ] || ng "path traversal"
# 壊れたもの・途中で切れたもの（落ちずに、失敗を返す）
head -c 3000 a-gnu.tar.gz > cut.tar.gz; mkdir c1; ./tar xf cut.tar.gz -C c1 2>/dev/null && ng "truncated gzip accepted"
head -c 5000 a-gnu.tar > cut.tar; mkdir c2; ./tar xf cut.tar -C c2 2>/dev/null && ng "truncated tar accepted"
head -c 4096 /dev/urandom > junk; ./tar tf junk 2>/dev/null && ng "junk accepted"
# gunzip・zcat
cp a-gnu.tar.gz g.tar.gz; ./gunzip g.tar.gz && cmp -s g.tar a-gnu.tar && [ ! -e g.tar.gz ] || ng "gunzip"
cp a-gnu.tar.gz k.tgz; ./gunzip -k k.tgz && cmp -s k.tar a-gnu.tar && [ -e k.tgz ] || ng "gunzip -k .tgz"
./zcat a-gnu.tar.gz | cmp -s - a-gnu.tar || ng "zcat"
./gunzip -c < a-gnu.tar.gz | cmp -s - a-gnu.tar || ng "gunzip stdin"
cp a-gnu.tar.gz e.tar.gz; : > e.tar; ./gunzip e.tar.gz 2>/dev/null && ng "gunzip overwrote"
[ $fail -eq 0 ] && echo "tar test: すべて成功" || echo "tar test: NG ($fail)"
exit $fail
