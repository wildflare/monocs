#!/bin/sh
# os/libc/wctype.c を、すべての文字（U+0000〜U+10FFFF と WEOF）で Linux の glibc（C.UTF-8）と比べる
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
names="iswalpha iswupper iswlower iswpunct iswprint iswspace iswblank iswcntrl iswdigit iswxdigit iswalnum iswgraph towupper towlower wctype iswctype wctrans towctrans"
defs=""
for n in $names; do defs="$defs -D$n=monocs_$n"; done
gcc -std=c23 -O1 -g -fsanitize=address,undefined $defs -c -o "$T/wctype.o" "$R/os/libc/wctype.c"
cat > "$T/t.c" <<'C'
#include <locale.h>
#include <stdio.h>
#include <wctype.h>
#define F(n) int monocs_##n(wint_t);
F(iswalpha) F(iswupper) F(iswlower) F(iswpunct) F(iswprint) F(iswspace) F(iswblank) F(iswcntrl) F(iswdigit)
F(iswxdigit) F(iswalnum) F(iswgraph)
wint_t monocs_towupper(wint_t); wint_t monocs_towlower(wint_t);
wctype_t monocs_wctype(const char *); int monocs_iswctype(wint_t, wctype_t);
wctrans_t monocs_wctrans(const char *); wint_t monocs_towctrans(wint_t, wctrans_t);
int main(void)
{
    setlocale(LC_ALL, "C.UTF-8");
    static const char *cls[] = {"alnum","alpha","blank","cntrl","digit","graph","lower","print","punct","space","upper","xdigit"};
    long bad = 0, n = 0;
    for (unsigned c = 0; c <= 0x110000; c++) {
        wint_t w = c == 0x110000 ? WEOF : c;
#define C(f) if (!!f(w) != !!monocs_##f(w)) { if (bad++ < 10) printf("U+%04X " #f " %d %d\n", c, !!f(w), !!monocs_##f(w)); }
        C(iswalpha) C(iswupper) C(iswlower) C(iswpunct) C(iswprint) C(iswspace) C(iswblank) C(iswcntrl)
        C(iswdigit) C(iswxdigit) C(iswalnum) C(iswgraph)
        if (towupper(w) != monocs_towupper(w) || towlower(w) != monocs_towlower(w)) {
            if (bad++ < 10) printf("U+%04X tow %x/%x %x/%x\n", c, towupper(w), monocs_towupper(w), towlower(w), monocs_towlower(w));
        }
        for (int i = 0; i < 12; i++)
            if (!!iswctype(w, wctype(cls[i])) != !!monocs_iswctype(w, monocs_wctype(cls[i]))) bad++;
        if (towctrans(w, wctrans("toupper")) != monocs_towctrans(w, monocs_wctrans("toupper"))) bad++;
        n++;
    }
    printf("%ld characters, %ld differences\n", n, bad);
    return bad != 0;
}
C
gcc -std=c23 -O1 -g -fsanitize=address,undefined -o "$T/t" "$T/t.c" "$T/wctype.o"
"$T/t"
