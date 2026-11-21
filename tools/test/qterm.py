# QEMU の Monocs を端末の代わりに動かす（テスト用）
#   qterm.py KERNEL IMAGE ROWS COLS STEP...
#   環境変数 QEMU_EXTRA: QEMU に足す引数（空白で区切る。I2C の機器をつなぐ -device など）
#   STEP: "wait:秒" | "keys:文字列（\\x1b などのエスケープ可）" | "screen" | "until:文字列:秒"
# 端末の大きさの問い合わせ（ESC [ 6 n）に ROWS・COLS で答え、出力を vt.py で再現する
import os, sys, subprocess, time, select, fcntl
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vt import VT

kernel, image, rows, cols = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
steps = sys.argv[5:]
q = subprocess.Popen(['qemu-system-aarch64', '-M', 'raspi3b', '-kernel', kernel, '-serial', 'stdio',
                      '-display', 'none', '-drive', f'file={image},if=sd,format=raw']
                     + os.environ.get('QEMU_EXTRA', '').split(),
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
fl = fcntl.fcntl(q.stdout, fcntl.F_GETFL)
fcntl.fcntl(q.stdout, fcntl.F_SETFL, fl | os.O_NONBLOCK)
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
            return
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
