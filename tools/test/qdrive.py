# QEMU の Monocs にコマンドを順に送り、プロンプトを待つ。出力を表示する
#   qdrive.py KERNEL IMG [TIMEOUT] -- CMD...（CMD の \x03 は Ctrl-C）
import os, subprocess, sys, time, select
kernel, img = sys.argv[1], sys.argv[2]
args = sys.argv[3:]
tmo = 600
if args and args[0] != '--':
    tmo = int(args[0]); args = args[1:]
args = args[1:] if args and args[0] == '--' else args
p = subprocess.Popen(["qemu-system-aarch64", "-M", "raspi3b", "-kernel", kernel, "-serial", "stdio",
                      "-display", "none", "-drive", f"file={img},if=sd,format=raw"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
buf = b""
def until(pat, timeout):
    global buf
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([p.stdout], [], [], 0.5)
        if r:
            d = os.read(p.stdout.fileno(), 65536)
            if not d: return False
            sys.stdout.write(d.decode(errors="replace").replace("\r", "")); sys.stdout.flush()
            buf += d
            if pat in buf[-64:]:
                buf = b""; return True
    return False
until(b"> ", 60)
for c in args:
    c = c.encode().decode('unicode_escape')
    p.stdin.write((c if c == "\x03" else c + "\r").encode()); p.stdin.flush()
    if not until(b"> ", tmo):
        print("\n*** TIMEOUT", flush=True); break
p.kill()
