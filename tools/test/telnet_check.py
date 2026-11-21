# telnet（services/telnetd.c）を確かめる: 挨拶、コマンド、画面の大きさ（NAWS 120x40）、Ctrl-C、200KB の表示。
# 先に device/ts.c・loop.c・big.c を実機の /tmp に送っておく（hwcheck.sh が送る）
import socket, time, select, sys
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
s = socket.create_connection(('192.168.7.2', 23), timeout=5)
buf = bytearray()
def pump(t=0.5, until=None):
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([s], [], [], 0.05)
        if not r: 
            if until and until in buf: return
            continue
        d = s.recv(65536)
        if not d: break
        # 交渉に答える
        i = 0
        while i < len(d):
            if d[i] == IAC and i + 2 < len(d) + 1 and i + 1 < len(d):
                cmd = d[i + 1]
                if cmd in (DO, DONT, WILL, WONT) and i + 2 < len(d):
                    opt = d[i + 2]
                    if cmd == DO and opt == 31:
                        s.sendall(bytes([IAC, WILL, 31, IAC, SB, 31, 0, 120, 0, 40, IAC, SE]))
                    elif cmd == WILL: s.sendall(bytes([IAC, DO, opt]))
                    i += 3; continue
                if cmd == IAC: buf.append(IAC); i += 2; continue
                i += 2; continue
            buf.append(d[i]); i += 1
        if until and until in buf: return
pump(1.0, b'> ')
print('banner:', bytes(buf).decode(errors='replace').strip()[:80]); buf.clear()
s.sendall(b' uptime\r\n'); pump(2, b'> ')
print('uptime:', bytes(buf).decode(errors='replace').split('\n')[1].strip()); buf.clear()
s.sendall(b' /tmp/ts\r\n'); pump(5, b'> ')
print('term:', ([l for l in bytes(buf).decode(errors='replace').split('\n') if 'rows=' in l] or ['?'])[-1].strip()); buf.clear()
s.sendall(b' /tmp/loop\r\n'); time.sleep(1.5); s.sendall(b'\x03'); pump(3, b'> ')
print('ctrl-c:', '^C' in bytes(buf).decode(errors='replace')); buf.clear()
t = time.time(); s.sendall(b' /tmp/big\r\n'); pump(30, b'03199'); pump(3, b'> ')
print('big: %d bytes in %.3f s, last ok=%s' % (len(buf), time.time() - t, b'03199 ' in buf and buf.count(b'\n') >= 3200))
s.close()
