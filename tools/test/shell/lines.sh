# tools/test/shell.sh が bash と Monocs で流して比べる行（1 行ずつ。どの行も o.txt に書く）
export A=hello; echo $A >> o.txt
echo "${A}x" '$A' \$A "a\$b" >> o.txt
echo "[$A]" [${A}] pre$A.post >> o.txt
false; echo $? >> o.txt
true; echo $? >> o.txt
false || echo or $? >> o.txt
false && echo no; echo and $? >> o.txt
unset A; echo "[$A]" >> o.txt
echo "[$NOPE_MONOCS]" x${NOPE_MONOCS}y >> o.txt
export B=; echo x $B y "$B" z >> o.txt
echo $HOME >> o.txt
nosuchcommand_monocs 2> /dev/null; echo $? >> o.txt
export C=a; export C="${C}b"; echo $C >> o.txt
echo $ "$" '$?' $1x >> o.txt
export PATH=/root/test:$PATH
X="a  b   c"; args $X >> o.txt
args "$X" >> o.txt
args $NOPE_MONOCS "" '' >> o.txt
Y=" lead  trail "; args x${Y}y >> o.txt
Z=shellvar; echo $Z >> o.txt
args -e Z >> o.txt
Z=tmp args -e Z >> o.txt
echo $Z >> o.txt
export Z; args -e Z >> o.txt
unset Z; args -e Z >> o.txt
A=1 B=2 args -e B >> o.txt
echo $(echo hi) "$(echo a  b)" >> o.txt
args $(echo 1 2) "$(echo 3 4)" >> o.txt
echo `echo back` "`echo q`" >> o.txt
echo "x$(echo "in ner")y" >> o.txt
V=$(false); echo $? >> o.txt
echo $(echo $(echo deep)) >> o.txt
W=$(cd /; echo w); echo $W >> o.txt
echo $(export Q=1) "[$Q]" >> o.txt
for i in 1 2 3; do echo i$i >> o.txt; done
for f in b a c; do if [ $f = a ]; then echo is-a >> o.txt; elif [ $f = b ]; then echo is-b >> o.txt; else echo other-$f >> o.txt; fi; done
n=0; while [ $n -lt 3 ]; do n=$((n+1)); done; echo n=$n >> o.txt
n=5; until [ $n -le 2 ]; do n=$((n-1)); done; echo until=$n >> o.txt
for i in 1 2 3 4; do if [ $i = 3 ]; then break; fi; echo b$i >> o.txt; done
for i in 1 2 3; do if [ $i = 2 ]; then continue; fi; echo c$i >> o.txt; done
for i in 1 2; do for j in a b; do if [ $j = b ]; then continue 2; fi; echo $i$j >> o.txt; done; done
for i in 1 2; do for j in a b; do break 2; done; echo never >> o.txt; done; echo after-break2 >> o.txt
echo $((1+2*3)) $((7/2)) $((7%3)) $((-4+1)) $((2<3)) $((1&&0)) $((5>3?10:20)) $((1<<4)) $((0x10)) $(( (2+3)*4 )) >> o.txt
x=7; echo $((x*2)) $(($x+1)) $((x==7)) $((x!=7)) $((x>=8||x<=7)) $((~0)) $((6&3)) $((6|3)) $((6^3)) >> o.txt
unset U; echo ${U:-def} ${U-d2} "${U:+alt}" [${#HOME}] >> o.txt
U=; echo [${U:-e}] [${U-f}] [${U:+g}] [${U+h}] >> o.txt
V=hello.tar.gz; echo ${V%.gz} ${V%%.*} ${V#*.} ${V##*.} ${#V} >> o.txt
unset W; echo ${W:=set1} $W "${W:-x y}" >> o.txt
D="a b"; args ${D:-z} "${D:-z}" ${NOPE_MONOCS:-1 2} >> o.txt
if true; then echo t1 >> o.txt; fi
if false; then echo no; else echo e1 >> o.txt; fi
if false; then echo no; fi; echo if-status $? >> o.txt
! false && echo not >> o.txt
! true; echo not-status $? >> o.txt
[ -d / ] && echo isdir >> o.txt
[ -f /nonexist_monocs ] || echo nofile >> o.txt
test 3 -gt 2 -a 1 -eq 1 && echo t-and >> o.txt
[ "" ] || echo empty-false >> o.txt
[ abc = abc ] && [ abc != abd ] && echo streq >> o.txt
[ ! -e /nonexist_monocs ] && [ -n x ] && [ -z "" ] && echo t-misc >> o.txt
if [ 1 -eq 1 ]
then
  echo multi-then >> o.txt
fi
for k in x y
do
  echo multi-$k >> o.txt
done
echo a \
b >> o.txt
echo "line1
line2" >> o.txt
echo '*' "*" \* >> o.txt
mkdir -p gt; touch gt/b.c gt/a.c gt/c.h gt/.hid.c
echo gt/*.c >> o.txt
echo gt/?.h gt/[ab].c >> o.txt
echo gt/*.none >> o.txt
echo "gt/*.c" 'gt/*.c' gt/\*.c >> o.txt
for f in gt/*.c; do echo f=$f >> o.txt; done
G='gt/*.h'; echo $G "$G" >> o.txt
echo done-$? # 注釈 >> o.txt
echo comment-ok >> o.txt # 注釈
case abc in a*) echo case-a >> o.txt ;; *) echo case-other >> o.txt ;; esac
case x.c in *.h|*.c) echo c-or-h >> o.txt ;; esac
case "a b" in "a b") echo quoted-case >> o.txt ;; esac
case z in a) echo no ;; esac; echo case-none $? >> o.txt
P='*'; case abc in $P) echo pat-var >> o.txt ;; esac
case 'xy' in "*") echo no ;; x?) echo lit-star >> o.txt ;; esac
case foo in
  f*)
    echo multi-case >> o.txt
    ;;
esac
greet() { echo hello-$1-$# >> o.txt; }
greet world
greet a b c
add() { echo $(($1 + $2)); }; echo sum=$(add 3 4) >> o.txt
f() { local L=in; echo $L >> o.txt; return 3; }; L=out; f; echo ret=$? L=$L >> o.txt
args2() { args "$@"; args "$*"; args $@; }; args2 'a b' c >> o.txt
fn2() { for x in "$@"; do echo arg=$x >> o.txt; done; }; fn2 1 '2 3'
fn3() { shift; echo $1 $# >> o.txt; }; fn3 a b c
rec() { if [ $1 -gt 0 ]; then rec $(($1-1)); echo r$1 >> o.txt; fi; }; rec 3
multi()
{
  echo multi-fn $1 >> o.txt
}
multi ok
(cd /; X=sub; echo in-sub) >> o.txt; echo "[$X]" >> o.txt
{ echo g1; echo g2; } >> o.txt
( echo s1; echo s2 ) | cat >> o.txt
echo {a,b,c} x{1,2}y >> o.txt
echo {1..5} {a..e} {5..1} {01..03} {1..10..3} >> o.txt
echo {a,b}{1,2} "{a,b}" \{a,b\} {single} >> o.txt
echo ~ ~/x "~" >> o.txt
V=hello-world; echo ${V/o/0} ${V//o/0} ${V/#h/H} ${V/%d/D} ${V/l*/X} >> o.txt
echo ${V:0:5} ${V:6} ${V: -5} ${V:2:-2} >> o.txt
echo $# "[$@]" "[$*]" >> o.txt
set -- p q r; echo $# $2 "$@" >> o.txt
set --; echo $# >> o.txt
cat <<EOF >> o.txt
here $V
  indented "q" 'q'
EOF
cat <<'EOF' >> o.txt
no $V expand
EOF
unset -f greet; greet x 2> /dev/null; echo unset-f $? >> o.txt
: ; echo colon $? >> o.txt
sh script.sh x y >> o.txt
time false 2> /dev/null; echo time $? >> o.txt
time echo timed >> o.txt
time ! false; echo timenot $? >> o.txt
time echo piped | cat >> o.txt
(time -p true) 2> te.txt; grep -c "^real " te.txt >> o.txt
(time true) 2> te2.txt; grep -c "^user" te2.txt >> o.txt
mkdir -p many; touch many/a{1..600}.x many/b{1..600}.x; echo many/*.x | wc -w >> o.txt
n=0; for f in many/*.x; do n=$((n+1)); done; echo for $n >> o.txt
ls many/*.x | wc -l >> o.txt
rm many/*.x; ls many | wc -l >> o.txt
export EV=outer; env EV=inner EW=w sh -c 'echo $EV $EW' >> o.txt; echo after $EV "[$EW]" >> o.txt
env -u EV sh -c 'echo "[$EV]"' >> o.txt; env -i sh -c 'echo "[$EV]"' >> o.txt; env EV=1 | grep -c '^EV=1$' >> o.txt
env -- EV=2 sh -c 'echo $EV' >> o.txt; env true; echo $? >> o.txt
{ sh -c 'ls /nonexist_monocs' 2> ie.txt; } > /dev/null 2>&1; wc -l < ie.txt >> o.txt
{ sh -c 'echo inner' > io.txt; } > /dev/null; cat io.txt >> o.txt
i=0; : > long.sh; while [ $i -lt 300 ]; do echo "x=$i; y=\$((x * 2)); [ \$y -ge 0 ] && : ; echo \"\$x\" > /dev/null" >> long.sh; i=$((i+1)); done; echo 'echo last $x $y' >> long.sh; sh long.sh >> o.txt; echo long $? >> o.txt
