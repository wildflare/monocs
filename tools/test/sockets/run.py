# ソケットの試験（Pi 5 で動かす）: Monocs の client.c・server.c の相手をする。
#   python3 tools/test/sockets/run.py [WEB_HOST]   （実機に新しいカーネルとヘッダーを入れてから）
import os, socket, socketserver, subprocess, sys, threading, time
R = os.path.dirname(os.path.abspath(__file__))
NOS = [sys.executable, os.path.join(R, '..', '..', 'uart', 'nos.py')]
# Wi-Fi で（NOS_HOST があれば）その相手と、そこへ届く Pi 5 のアドレス。なければ USB のアドレス
MONOCS = os.environ.get('NOS_HOST') or '192.168.7.2'
def _local_addr(peer):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect((peer, 9))
    a = s.getsockname()[0]
    s.close()
    return a
PI = _local_addr(MONOCS) if os.environ.get('NOS_HOST') else '192.168.7.1'
TCP_PORT, UDP_PORT, CLOSED_PORT, SERVER_PORT = 9007, 9009, 9011, 9008

class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        while True:
            d = self.request.recv(65536)
            if not d:
                break
            self.request.sendall(d)

class TS(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

tcp = TS((PI, TCP_PORT), Echo)
threading.Thread(target=tcp.serve_forever, daemon=True).start()
udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
udp.bind((PI, UDP_PORT))
def udp_echo():
    while True:
        d, a = udp.recvfrom(2048)
        udp.sendto(d, a)
threading.Thread(target=udp_echo, daemon=True).start()

def nos(*args, timeout=300):
    return subprocess.run(NOS + list(args), capture_output=True, text=True, timeout=timeout).stdout

for f in ('client.c', 'server.c'):
    nos('push', os.path.join(R, f), '/tmp/sock_' + f)
web = sys.argv[1] if len(sys.argv) > 1 else ''
print('--- client (Monocs -> Pi 5)')
out = nos('cmd', f'cc -o /tmp/sock_client /tmp/sock_client.c && /tmp/sock_client {PI} {TCP_PORT} {UDP_PORT} {CLOSED_PORT} {web}')
print('\n'.join(l for l in out.splitlines() if not l.startswith(('/', ' run'))))

print('--- server (Pi 5 -> Monocs, 3 clients at once)')
srv = subprocess.Popen(NOS + ['cmd', f'cc -o /tmp/sock_server /tmp/sock_server.c && /tmp/sock_server {SERVER_PORT}'], stdout=subprocess.PIPE, text=True)
for _ in range(100):    # 待ち受けるまで
    try:
        socket.create_connection((MONOCS, SERVER_PORT), timeout=1).close()
        break
    except OSError:
        time.sleep(0.5)
ok = []
def client(k):
    s = socket.create_connection((MONOCS, SERVER_PORT), timeout=20)
    data = bytes((i * 13 + k) & 255 for i in range(300000))
    def send():
        s.sendall(data)
    t = threading.Thread(target=send)
    t.start()
    got = b''
    while len(got) < len(data):
        d = s.recv(65536)
        if not d:
            break
        got += d
    t.join()
    s.close()
    ok.append(got == data)
ts = [threading.Thread(target=client, args=(k,)) for k in range(3)]
for t in ts:
    t.start()
for t in ts:
    t.join()
print('3 clients echo 300KB each:', 'match' if ok == [True] * 3 else ok)
q = socket.create_connection((MONOCS, SERVER_PORT), timeout=5)
q.sendall(b'quit\n')
time.sleep(0.5)
q.close()
out, _ = srv.communicate(timeout=60)
print('\n'.join(l for l in out.splitlines() if l.startswith('server')))
