# アプリの TCP（samples/tcp_echo.c）を確かめる: USB シリアルで作って動かし、2MB を送って大文字で返るか、Ctrl-C で止まるか
import serial, socket, time, os, threading, glob
p = serial.Serial(glob.glob('/dev/serial/by-id/usb-*console*')[0], timeout=0.2)
p.write(b' cc -o /tmp/tcp_echo /usr/share/samples/tcp_echo.c && /tmp/tcp_echo\r'); time.sleep(2.5); print(p.read(4096).decode(errors='replace').strip().splitlines()[-1])
data = os.urandom(2 << 20).hex().encode()[:2 << 20].lower()
hello = b"hello from Monocs\n"   # samples/tcp_echo.c がつないだときに送る挨拶
s = socket.create_connection(('192.168.7.2', 7000), timeout=10); got = bytearray()
def rd():
    while len(got) < len(data) + len(hello):
        d = s.recv(65536)
        if not d: break
        got.extend(d)
t = threading.Thread(target=rd); t0 = time.time(); t.start(); s.sendall(data); t.join(); dt = time.time() - t0
print('app echo 2 MB in %.2f s, match=%s' % (dt, bytes(got) == hello + data.upper())); s.close()
t0 = time.time(); p.write(b'\x03'); out = b''
while b'/> ' not in out and time.time() - t0 < 5: out += p.read(4096)
print('ctrl-c ->prompt %.3f s' % (time.time() - t0))
