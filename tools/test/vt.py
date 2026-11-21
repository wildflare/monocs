# 小さな端末の再現（テスト用）: CUP・ED・EL・SGR（無視）・?1049h/l・UTF-8 の幅（東アジアの全角は 2）
import unicodedata, re

def width(ch):
    if unicodedata.combining(ch):
        return 0
    return 2 if unicodedata.east_asian_width(ch) in ('W', 'F') else 1

class VT:
    def __init__(self, rows=24, cols=80):
        self.rows, self.cols = rows, cols
        self.main = [[' '] * cols for _ in range(rows)]
        self.alt = None
        self.y = self.x = 0
        self.cursor = True

    @property
    def scr(self):
        return self.alt if self.alt is not None else self.main

    def feed(self, data: bytes):
        s = data.decode('utf-8', errors='replace')
        i = 0
        while i < len(s):
            c = s[i]
            if c == '\x1b':
                m = re.match(r'\x1b\[([?>]?)([0-9;]*)([A-Za-z@~])', s[i:])
                if m:
                    self.csi(m.group(1), m.group(2), m.group(3))
                    i += m.end()
                    continue
                if i + 1 < len(s) and s[i + 1] == '7':
                    self.dec_saved = (self.y, self.x)
                elif i + 1 < len(s) and s[i + 1] == '8':
                    self.y, self.x = getattr(self, 'dec_saved', (self.y, self.x))
                if i + 1 < len(s) and s[i + 1] in '78=>()':
                    i += 3 if s[i + 1] in '()' else 2
                    continue
                i += 1
                continue
            if c in '\r\n\b':
                self.pending = False
            if c == '\r':
                self.x = 0
            elif c == '\n':
                if self.y == self.rows - 1:
                    self.scr.pop(0)
                    self.scr.append([' '] * self.cols)
                else:
                    self.y += 1
            elif c == '\b':
                self.x = max(self.x - 1, 0)
            elif c in '\a\x00':
                pass
            else:
                w = width(c)
                if w == 0:
                    pass
                else:
                    if self.x + w > self.cols or getattr(self, 'pending', False):
                        self.pending = False
                        self.x = 0
                        if self.y == self.rows - 1:
                            self.scr.pop(0); self.scr.append([' '] * self.cols)
                        else:
                            self.y += 1
                    self.scr[self.y][self.x] = c
                    if w == 2 and self.x + 1 < self.cols:
                        self.scr[self.y][self.x + 1] = ''
                    self.x += w
                    if self.x >= self.cols:
                        self.x = self.cols - 1
                        self.pending = True     # 右端の後の折り返しを保留する（xterm と同じ）
            i += 1

    def csi(self, priv, params, final):
        self.pending = False
        p = [int(v) if v else 0 for v in params.split(';')] if params else []
        if priv == '?':
            if 1049 in p:
                if final == 'h':
                    self.saved = (self.y, self.x)
                    self.alt = [[' '] * self.cols for _ in range(self.rows)]
                else:
                    self.alt = None
                    self.y, self.x = getattr(self, 'saved', (self.y, self.x))
            if 25 in p:
                self.cursor = final == 'h'
            return
        if priv == '>':
            return
        if final == 'H':
            self.y = max((p[0] if len(p) > 0 and p[0] else 1) - 1, 0)
            self.x = max((p[1] if len(p) > 1 and p[1] else 1) - 1, 0)
        elif final in 'ABCD':
            n = p[0] if p and p[0] else 1
            if final == 'A': self.y = max(self.y - n, 0)
            if final == 'B': self.y = min(self.y + n, self.rows - 1)
            if final == 'C': self.x = min(self.x + n, self.cols - 1)
            if final == 'D': self.x = max(self.x - n, 0)
        elif final == 'J':
            if (p[0] if p else 0) == 2:
                for row in self.scr:
                    row[:] = [' '] * self.cols
        elif final == 'K':
            for x in range(self.x, self.cols):
                self.scr[self.y][x] = ' '
        elif final == 'n' and p and p[0] == 6:
            pass

    def text(self):
        return '\n'.join(''.join(r).rstrip() for r in self.scr)
