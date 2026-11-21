// termcap の関数（Linux の ncurses の termcap.h と同じ名前・形。os/libc/termcap.c）
// Monocs の端末はどれも xterm と同じに扱い、tgetent はどの名前でも xterm-256color を選ぶ。文字列は ncurses と同じく
// terminfo の形で、tgoto・tparm で解く
#ifndef _TERMCAP_H
#define _TERMCAP_H 1

#ifdef __cplusplus
extern "C" {
#endif

extern char PC;
extern char *UP;
extern char *BC;
extern short ospeed;

int tgetent(char *bp, const char *name);
int tgetflag(const char *id);
int tgetnum(const char *id);
char *tgetstr(const char *id, char **area);
char *tgoto(const char *cap, int col, int row);
int tputs(const char *str, int affcnt, int (*putc)(int));
char *tparm(const char *str, ...);
char *tiparm(const char *str, ...);

#ifdef __cplusplus
}
#endif

#endif
