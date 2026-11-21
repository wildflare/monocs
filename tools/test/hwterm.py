# 実機の Monocs に USB シリアルでつなぎ、端末の代わりをする（テスト用）
#   hwterm.py ROWS COLS STEP...
#   STEP: "wait:秒" | "keys:文字列（\\x1b などのエスケープ可）" | "screen" | "until:文字列:秒"
# 端末の大きさの問い合わせ（ESC [ 6 n）に ROWS・COLS で答え、出力を vt.py で再現する
import os, sys, subprocess, time, select, fcntl
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vt import VT

rows, cols = int(sys.argv[1]), int(sys.argv[2])
steps = sys.argv[3:]
import termios, glob
fd = os.open(glob.glob('/dev/serial/by-id/usb-*console*')[0], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd)
a[0] = a[1] = a[3] = 0
a[2] |= termios.CS8 | termios.CREAD | termios.CLOCAL
termios.tcsetattr(fd, termios.TCSANOW, a)
class P:
    def __init__(s, fd): s.fd = fd
    def read(s, n):
        try: return os.read(s.fd, n)
        except BlockingIOError: return b''
    def write(s, b): os.write(s.fd, b)
    def flush(s): pass
    def fileno(s): return s.fd
class Q:
    stdout = P(fd); stdin = P(fd)
    def kill(s): os.close(fd)
q = Q()
vt = VT(rows, cols)
raw = bytearray()

def pump(t):
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([q.stdout], [], [], 0.02)
        if not r:
            continue
        d = q.stdout.read(65536)
        if not d:
            continue
        before = raw.count(b'\x1b[6n')
        raw.extend(d)
        for _ in range(raw.count(b'\x1b[6n') - before):   # 問い合わせが読み込みの途中で分かれても答える
            q.stdin.write(f'\x1b[{rows};{cols}R'.encode())
            q.stdin.flush()

last_keys = [0, 0.0]   # 最後に keys を送り始めたときの raw の位置と時刻（until はそこから探す）

def send(s):
    last_keys[0], last_keys[1] = len(raw), time.time()
    b = s.encode('utf-8').decode('unicode_escape').encode('latin1') if '\\' in s else s.encode('utf-8')
    for i in range(0, len(b), 16):
        q.stdin.write(b[i:i + 16])
        q.stdin.flush()
        pump(0.02)

pump(0)
for st in steps:
    kind, _, arg = st.partition(':')
    if kind == 'wait':
        pump(float(arg))
    elif kind == 'keys':
        send(arg)
    elif kind == 'until':
        # 最後の keys を送り始めてから現れたかを探す（速い応答は送っている途中に届く）。時間も送り始めから
        text, _, t = arg.rpartition(':')
        start_pos, start_t = last_keys
        end = time.time() + float(t)
        while time.time() < end and text.encode() not in raw[start_pos:]:
            pump(0.02)
        ok = text.encode() in raw[start_pos:]
        print(f'[until {text!r} {time.time() - start_t:.2f}s: {"ok" if ok else "TIMEOUT"}]')
    elif kind == 'screen':
        vt = VT(rows, cols)
        vt.feed(bytes(raw))
        print('----- screen' + (' (' + arg + ')' if arg else '') + f' cursor=({vt.y},{vt.x}) alt={vt.alt is not None}')
        print(vt.text())
        print('-----')
    elif kind == 'rawtail':
        print(repr(bytes(raw[-int(arg or 400):])))
q.kill()
