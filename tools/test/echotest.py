# カーネルの TCP エコー（ポート 7）で、USB イーサネットの速さと一致を確かめる: echotest.py [MB]
import socket, os, threading, time, sys
n = int(sys.argv[1]) if len(sys.argv) > 1 else 20
data = os.urandom(n * 1024 * 1024)
s = socket.create_connection(('192.168.7.2', 7), timeout=10)
got = bytearray()
def rd():
    while len(got) < len(data):
        d = s.recv(1 << 16)
        if not d: break
        got.extend(d)
t = threading.Thread(target=rd); t0 = time.time(); t.start()
s.sendall(data); t.join(); dt = time.time() - t0
s.close()
print('echo %d MB in %.2f s = %.1f MB/s each way, match=%s' % (n, dt, len(data) / dt / 1e6, bytes(got) == data))
