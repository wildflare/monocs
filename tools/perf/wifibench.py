#!/usr/bin/env python3
# Wi-Fi の速さを測る（2026-11-22）。Monocs で tools/perf/wifibench.c を動かし、Pi 5 から送る・受ける。
#   python3 tools/perf/wifibench.py [send|recv] [MB] [ADDR]   （ssh の monocs で動かし、ADDR（既定は NOS_HOST、なければ USB の 192.168.7.2）につなぐ）
#   send: Monocs → Pi 5（Monocs が送る）、recv: Pi 5 → Monocs（Monocs が受ける）
import os, socket, subprocess, sys, time

mode = sys.argv[1] if len(sys.argv) > 1 else "send"
mb = float(sys.argv[2]) if len(sys.argv) > 2 else 4
host = sys.argv[3] if len(sys.argv) > 3 else os.environ.get("NOS_HOST", "192.168.7.2")   # Monocs のアドレス
size = int(mb * 1024 * 1024)
p = subprocess.Popen(["ssh", "monocs", "run /root/work/wifibench.c"], stdout=subprocess.PIPE, text=True)
line = p.stdout.readline()
if "listening" not in line:
    print(line, end=""); print(p.stdout.read()); sys.exit(1)
s = socket.create_connection((host, 5001), timeout=60)
t0 = time.time()
if mode == "send":
    s.sendall(f"send {size}\n".encode())
    got = 0
    while True:
        d = s.recv(65536)
        if not d:
            break
        got += len(d)
else:
    s.sendall(b"recv\n")
    chunk = b"y" * 65536
    left = size
    while left > 0:
        k = min(left, len(chunk))
        s.sendall(chunk[:k])
        left -= k
    s.shutdown(socket.SHUT_WR)
    got = int(s.recv(64).decode().strip() or 0)
t = time.time() - t0
s.close()
print(f"pi5: {mode} {got} bytes in {t:.3f} s ({got / t / 1024:.0f} KB/s)")
print(p.stdout.read(), end="")
p.wait()
