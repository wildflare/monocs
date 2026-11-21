# telnet で Monocs（192.168.7.2）につなぎ、端末の代わりをする（テスト用）: tnterm.py ROWS COLS STEP...
#   STEP: wait:秒 | keys:文字列 | until:文字列:秒 | screen:名前 | naws:行:桁
import os, sys, socket, time, select
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vt import VT
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
rows, cols = int(sys.argv[1]), int(sys.argv[2])
s = socket.create_connection(('192.168.7.2', 23), timeout=5)
raw = bytearray()

def naws(r, c):
    s.sendall(bytes([IAC, SB, 31, c >> 8, c & 255, r >> 8, r & 255, IAC, SE]))

def pump(t):
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([s], [], [], 0.02)
        if not r:
            continue
        d = s.recv(65536)
        if not d:
            return
        i = 0
        while i < len(d):
            if d[i] == IAC and i + 1 < len(d):
                cmd = d[i + 1]
                if cmd in (DO, DONT, WILL, WONT) and i + 2 < len(d):
                    opt = d[i + 2]
                    if cmd == DO and opt == 31:
                        s.sendall(bytes([IAC, WILL, 31])); naws(rows, cols)
                    elif cmd == WILL:
                        s.sendall(bytes([IAC, DO, opt]))
                    i += 3
                    continue
                if cmd == IAC:
                    raw.append(IAC); i += 2; continue
                i += 2
                continue
            raw.append(d[i]); i += 1

pump(1)
last_keys = 0   # 最後に keys を送り始めたときの raw の位置（until はそこから探す。qterm.py と同じ。速い応答は送った直後に届く）
for st in sys.argv[3:]:
    kind, _, arg = st.partition(':')
    if kind == 'wait':
        pump(float(arg))
    elif kind == 'keys':
        b = arg.encode('utf-8').decode('unicode_escape').encode('latin1') if '\\' in arg else arg.encode()
        last_keys = len(raw)
        s.sendall(b.replace(b'\r', b'\r\0'))
        pump(0.05)
    elif kind == 'naws':
        r, c = arg.split(':'); rows, cols = int(r), int(c); naws(rows, cols); pump(0.1)
    elif kind == 'until':
        text, _, t = arg.rpartition(':')
        t0 = time.time(); n0 = last_keys
        while time.time() - t0 < float(t) and text.encode() not in raw[n0:]:
            pump(0.1)
        print(f'[until {text!r} {time.time() - t0:.1f}s: {"ok" if text.encode() in raw[n0:] else "TIMEOUT"}]')
    elif kind == 'screen':
        vt = VT(rows, cols); vt.feed(bytes(raw))
        print(f'----- screen ({arg}) {rows}x{cols} cursor=({vt.y},{vt.x}) alt={vt.alt is not None}')
        print(vt.text()); print('-----')
s.close()
