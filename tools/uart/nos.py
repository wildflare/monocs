#!/usr/bin/env python3
# Monocs を USB（CDC-ACM。既定）か UART（Pi 5 の /dev/ttyAMA0）から操作する。SD カードを差し替えずに、
# ファイルやカーネルを入れ替える。UART は起動の表示を見るときだけ使う（USB がなければ代わりに使う）。
#
#   nos.py cmd 'ls /'                       シェルのコマンドを実行して、出力を表示する（終了の値は、そのコマンドの
#                                           終了の値。httpd の /.status の status から。HTTP がなければ 0）
#   nos.py log                              カーネルの記録（dmesg）を HTTP で読む（打ち込まないので、使っている
#                                           最中の実機でもよい。HTTP がなければ dmesg を打つ）
#   nos.py push LOCAL REMOTE                ファイルを送る（HTTP の PUT。届かなければシェルの recv。大きさ・CRC32 で確かめる）
#   nos.py kernel [IMG]                     カーネル（既定は標準の slimcc 版 build/kslim/kernel8.img）を /boot/kernel8.img に送り
#                                           （/boot がない古い SD カードは /kernel8.img。隣に .sym があれば
#                                           kernel8.sym も。perf record の関数名）、
#                                           再起動して起動を待つ
#   nos.py run FILE.c [ARGS...]             C のファイルを /tmp に送り、run で動かす（その場でコンパイル。キャッシュあり）
#   nos.py pushdir LOCALDIR REMOTEDIR       ディレクトリの中身をまとめて送る（1 つの束にして送り、
#                                           Monocs 上で tools/uart/unpack.c が展開する）
#   nos.py reboot                           再起動して起動を待つ
#   nos.py poweroff                         書き出して止める（シェルの poweroff。その後は電源を入れ直すまで動かない）
#   nos.py wait                             プロンプトが出るまで待つ（動いていればすぐ戻る）
#   nos.py boot                             起動の表示を UART で見て、USB が使えるまで待つ（電源を入れ直すとき）
#
# kernel・reboot は、起動の表示を UART から読んで表示し（つながっていれば）、USB で開き直す。
# つなぐたびに Pi 5 の日時とタイムゾーンを送る（シェルの date。Monocs には時計がない）。
# 送るコマンドは前に空白を付ける（シェルの履歴に残らない）。
# ファイルは、Monocs の HTTP サーバー（http://192.168.7.2、os/net/httpd.c）に届けば HTTP の PUT で送り、
# HEAD で大きさを確かめる。届かなければ（または --serial）シェルの recv で送る。
# オプション: --usb（USB だけを使う）、--uart（UART を使う）、--port（端末を指定する）、
#             --serial（ファイルを HTTP ではなくコンソールで送る）、
#             --baud（UART の速さ。既定 3000000。Monocs の既定と同じ）、
#             --timeout（コマンドの完了を待つ秒数。既定 600）
#
# Wi-Fi で使う（2026-11-16。USB・UART の線なしで）: --host アドレス（か環境変数 NOS_HOST）。Monocs の sshd に ssh でつなぎ、
#   cmd は ssh の exec（シェルの 1 行。os/kernel/rexec.c。打ち込まないので、使っている最中の実機でもよい）、
#   push はふつうの sftp で送る（暗号化される。名前.tmp に送ってから入れ替える）。reboot は ssh がまたつながるまで待つ（起動の表示は見えない。dmesg で見る）。
#   kernel は試しの起動（os/kernel/tryboot.c）: kernel8.trial.img に置いて reboot tryboot、ssh が通って tryboot confirm
#   できたら kernel8.img に写す。通らなければ、ファームウェアが前のカーネルで起動し直す（約 2 分まで待つ）。鍵は ~/.ssh/id_ecdsa_monocs（NOS_SSH_KEY で変える。Monocs の sshd は Ed25519 を使えない）を
#   Monocs の /root/.ssh/authorized_keys に登録しておく。sshd は同時に 3 接続まで（対話の ssh を開いたままでも使える）。
#   --stop-app（NOS_STOP_APP=1）: コンソールで動いているアプリ（top など）に待たされたら、sshd が止めてから動かす。
#   --uart・--port を付けたときは、NOS_HOST があっても線を使う
# 送る手順（os/shell/shell.c の cmd_recv）: 「recv PATH SIZE CRC32 BLOCK」→「recv: ready」→ BLOCK バイト
# （UART は 4096、USB は 32768）ずつ送り、そのたびに「+」を待つ →「recv: ok」
import argparse
import codecs
import http.client
import os
import socket
import urllib.parse
import select
import sys
import termios
import time
import zlib

BLOCK = 4096        # UART で 1 回に送る量（受信バッファ 64KB に十分収まる）
USB_BLOCK = 32768   # USB で 1 回に送る量（recv の上限）
# USB の機器の名前（os/drivers/usb.c）
USB_PORTS = ['/dev/serial/by-id/usb-Monocs_Monocs_console-if00']
USB_PORT = USB_PORTS[0]


def usb_port():
    # 今ある USB のコンソール（なければ None）
    for p in USB_PORTS:
        if os.path.exists(p):
            return p
    return None
UART_PORT = '/dev/ttyAMA0'


class Console:
    def __init__(self, port, baud):
        self.port, self.baud = port, baud
        self.usb = port.startswith('/dev/serial/by-id/') or 'ttyACM' in port
        self.open()

    def open(self):
        port, baud = self.port, self.baud
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
        a = termios.tcgetattr(self.fd)
        a[0] = a[1] = a[3] = 0
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        if self.usb:
            a[2] |= termios.HUPCL  # 閉じたら DTR を下ろす（Monocs は端末が閉じたと分かり、USB への出力をやめる）
        speed = getattr(termios, 'B%d' % baud)
        a[4] = a[5] = speed
        a[6][termios.VMIN] = 0
        a[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        self.buf = b''

    def write(self, data):
        while data:
            n = os.write(self.fd, data)
            data = data[n:]

    def read_until(self, pats, timeout, echo=None, dec=None):
        # pats のどれかが現れるまで読む。見つかった位置までを返し、残りは self.buf に残す。
        # echo に出すときは、UTF-8 を読み込みの切れ目をまたいで解く（dec。1 文字が 2 回の読み込みに分かれると化けた）
        if echo and dec is None:
            dec = codecs.getincrementaldecoder('utf-8')(errors='replace')
        if isinstance(pats, bytes):
            pats = [pats]
        end = time.time() + timeout
        while True:
            for p in pats:
                i = self.buf.find(p)
                if i >= 0:
                    out = self.buf[:i + len(p)]
                    self.buf = self.buf[i + len(p):]
                    return out
            if time.time() > end:
                raise TimeoutError('timeout waiting for %r (got %r)' % (pats, self.buf[-200:]))
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                d = os.read(self.fd, 65536)
                if echo:
                    echo.write(dec.decode(d).replace('\r', ''))
                    echo.flush()
                self.buf += d

    def drain(self, quiet=0.2):
        # しばらく何も届かなくなるまで読み捨てる
        while True:
            r, _, _ = select.select([self.fd], [], [], quiet)
            if not r:
                break
            os.read(self.fd, 65536)
        self.buf = b''


PROMPT = b'> '


def sync(con):
    # プロンプトを出させる
    con.drain()
    con.write(b'\r')
    con.read_until(PROMPT, 10)
    con.drain()  # 余分なプロンプト（起動直後に受信バッファに残っていた文字などによる）を読み捨てる
    set_time(con)


def set_time(con):
    # Monocs には時計がないので、つなぐたびに Pi 5 の日時とタイムゾーンを送る（シェルの date。os/kernel/clock.c）。
    # date のない古いカーネルでは「コマンドがありません」になるだけ
    off = time.localtime().tm_gmtoff // 60
    tz = '%s%02d%02d' % ('-' if off < 0 else '+', abs(off) // 60, abs(off) % 60)
    run_cmd(con, 'date -s @%.3f -z %s' % (time.time(), tz), 10, echo=False)


def run_cmd(con, line, timeout, echo=True):
    # 前に空白を付けて送る（シェルの履歴に残らない。os/shell/lineedit.c。bash の ignorespace と同じ）
    con.write(b' ' + line.encode() + b'\r')
    # 入力の折り返し（送ったコマンドの行そのもの）を読み飛ばしてから、出力をプロンプトまで表示する
    con.read_until(line.encode() + b'\r\n', 10)
    # 折り返しと一緒に、出力の一部（全部のこともある）がもう届いて buf にある（3Mbps では多い）
    dec = codecs.getincrementaldecoder('utf-8')(errors='replace')
    if echo:
        sys.stdout.write(dec.decode(con.buf).replace('\r', ''))
        sys.stdout.flush()
    out = con.read_until(PROMPT, timeout, echo=sys.stdout if echo else None, dec=dec)
    if echo:
        print()
    return out


def last_status():
    # シェルが最後に動かした行の終了の値（httpd の /.status の status。2026-11-15 から）。わからなければ 0
    if not http_available():
        return 0
    import urllib.request
    try:
        st = urllib.request.urlopen('http://%s/.status' % HTTP_HOST, timeout=2).read().decode()
    except Exception:
        return 0
    for line in st.splitlines():
        if line.startswith('status '):
            return int(line.split()[1])
    return 0


def show_log():
    # カーネルの記録（GET /.log）。なければ None
    if not http_available():
        return None
    import urllib.request
    try:
        r = urllib.request.urlopen('http://%s/.log' % HTTP_HOST, timeout=5)
    except Exception:
        return None
    sys.stdout.write(r.read().decode(errors='replace'))
    sys.stdout.flush()
    return True


HTTP_HOST = '192.168.7.2'
use_http = None     # None: まだ調べていない


def http_available():
    global use_http
    if use_http is None:
        try:
            socket.create_connection((HTTP_HOST, 80), timeout=0.5).close()
            use_http = True
        except OSError:
            use_http = False
    return use_http


def check_idle(force):
    # 打ち込む前に、実機が使われていないかを見る（httpd の /.status。2026-11-14 から）。アプリが動いている・telnet・ssh で
    # だれかがつないでいるときに打ち込むと、その画面（mi で編集中のファイルなど）に文字が入る。閉じた直後のことも
    # あるので、3 秒は待ち直す。/.status のない古いカーネルや、HTTP が使えないときは調べない
    if force or os.environ.get('NOS_FORCE') == '1' or not http_available():
        return
    import urllib.request
    end = time.time() + 3
    while True:
        try:
            st = urllib.request.urlopen('http://%s/.status' % HTTP_HOST, timeout=2).read().decode()
        except Exception:
            return
        busy = [what for what, key in (('アプリが動いている', 'app 1'), ('telnet・ssh でつながっている', 'remote 1'))
                if key in st]
        if 'app ' not in st or not busy:
            return
        if time.time() > end:
            raise RuntimeError('実機は使われています（%s）。打ち込むとその画面に文字が入るので止めました。'
                               '終わってから流すか、--force（NOS_FORCE=1）で続けてください' % '・'.join(busy))
        time.sleep(0.5)


def http_push(local, remote, data):
    path = urllib.parse.quote(remote)
    t0 = time.time()
    step = 'connect'
    try:
        c = http.client.HTTPConnection(HTTP_HOST, 80, timeout=60)
        c.connect()
        step = 'PUT send'
        c.request('PUT', path, body=data, headers={'Content-Length': str(len(data))})
        step = 'PUT response'
        r = c.getresponse()
        body = r.read()
        c.close()
    except TimeoutError:
        # どこで止まったかを残す（2026-11-15。hwcheck でまれに出た「timed out」の調べのため）
        raise TimeoutError('PUT %s: timed out in %s after %.1f s' % (remote, step, time.time() - t0))
    if r.status not in (200, 201, 204):
        raise RuntimeError('PUT %s: %d %s' % (remote, r.status, body.decode(errors='replace').strip()))
    # 書けた大きさを確かめる
    try:
        c = http.client.HTTPConnection(HTTP_HOST, 80, timeout=30)
        c.request('HEAD', path)
        r = c.getresponse()
        r.read()
        c.close()
    except TimeoutError:
        raise TimeoutError('HEAD %s: timed out after %.1f s' % (remote, time.time() - t0))
    if r.status != 200 or int(r.getheader('Content-Length', '-1')) != len(data):
        raise RuntimeError('PUT %s: size mismatch' % remote)
    dt = time.time() - t0
    print('%s -> %s: %d bytes, %.1f s (%.0f KB/s, http)' % (local, remote, len(data), dt,
                                                           len(data) / 1024 / max(dt, 1e-6)))


def push(con, local, remote, data=None):
    # data を渡したときは、local は表示だけに使う
    if data is None:
        data = open(local, 'rb').read()
    if http_available():
        http_push(local, remote, data)
        return
    crc = zlib.crc32(data) & 0xFFFFFFFF
    block = USB_BLOCK if con.usb else BLOCK
    con.write((' recv %s %d %08x %d\r' % (remote, len(data), crc, block)).encode())
    r = con.read_until([b'recv: ready', b'usage:', b'recv: out of memory'], 10)
    if r.endswith(b'usage:'):
        # BLOCK を受け付けない古いカーネル: 4096 バイトずつの手順で送り直す
        con.read_until(PROMPT, 10)
        block = BLOCK
        con.write((' recv %s %d %08x\r' % (remote, len(data), crc)).encode())
        r = con.read_until([b'recv: ready', b'usage:', b'recv: out of memory'], 10)
    if not r.endswith(b'recv: ready'):
        raise RuntimeError('recv failed: %r' % r)
    t0 = time.time()
    for off in range(0, len(data), block):
        con.write(data[off:off + block])
        r = con.read_until([b'+\r\n', b'recv: timeout'], 10)
        if not r.endswith(b'+\r\n'):
            raise RuntimeError('recv failed: %r' % r)
    r = con.read_until([b'recv: ok', b'recv: crc', b'recv: cannot'], 60)
    line = r + con.read_until(b'\n', 5)
    con.read_until(PROMPT, 10)
    if b'recv: ok' not in r:
        raise RuntimeError(line.decode(errors='replace').strip())
    dt = time.time() - t0
    print('%s -> %s: %d bytes, %.1f s (%.0f KB/s)' % (local, remote, len(data), dt,
                                                     len(data) / 1024 / max(dt, 1e-6)))


def wait_usb(con, timeout):
    # USB の機器が（再び）現れるまで待って開き直し、プロンプトを出させる
    end = time.time() + timeout
    usb = con.port in USB_PORTS
    while not (usb_port() if usb else os.path.exists(con.port)):
        if time.time() > end:
            raise TimeoutError('timeout waiting for %s' % con.port)
        time.sleep(0.1)
    if usb:
        con.port = usb_port()
    time.sleep(0.3)  # udev が権限を整えるのを待つ
    con.open()
    con.buf = b''
    sync(con)


def show_boot(mon, timeout):
    # 起動の表示を UART から読んで表示し、シェルの案内（Monocs shell.）まで待つ
    out = mon.read_until([b'Monocs shell.'], timeout)
    lines = out.decode(errors='replace').replace('\r', '').split('\n')
    # 再起動の直前の雑音（空白など）を除き、最初の行から表示する
    for i, l in enumerate(lines):
        if 'Monocs:' in l:
            lines = lines[i:]
            break
    for l in lines:
        if l.strip():
            print('  | ' + l.rstrip())


def uart_monitor(baud):
    # 起動の表示を見るための UART（なければ None）
    if not os.path.exists(UART_PORT):
        return None
    mon = Console(UART_PORT, baud)
    mon.drain()
    return mon


def reboot(con):
    mon = uart_monitor(con.baud) if con.usb else con
    con.write(b' reboot\r')   # 前に空白（履歴に残らない）
    con.read_until(b'rebooting', 10)
    con.buf = b''
    if not con.usb:
        show_boot(con, 120)
        con.read_until(PROMPT, 30)
        print('booted')
        return
    # 切り離されて機器が消えるのを待つ（消える前に開き直さないように）
    os.close(con.fd)
    end = time.time() + 10
    while os.path.exists(con.port) and time.time() < end:
        time.sleep(0.05)
    if mon:
        show_boot(mon, 120)
        os.close(mon.fd)
    wait_usb(con, 120)
    print('booted')


def make_pack(local):
    # 束: 「パスの長さ、パス、中身の長さ、中身」の繰り返し（長さは 4 バイト、リトルエンディアン）
    pack = bytearray()
    count = 0
    for d, _, names in sorted(os.walk(local)):
        for n in sorted(names):
            path = os.path.join(d, n)
            rel = os.path.relpath(path, local).encode()
            data = open(path, 'rb').read()
            pack += len(rel).to_bytes(4, 'little') + rel + len(data).to_bytes(4, 'little') + data
            count += 1
    return pack, count


def pushdir(con, local, remote, timeout):
    pack, count = make_pack(local)
    run_cmd(con, 'mkdir /tmp', 10, echo=False)
    push(con, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'unpack.c'), '/tmp/unpack.c')
    push(con, '(%d files)' % count, '/pack.tmp', bytes(pack))
    run_cmd(con, 'cc -o /tmp/unpack /tmp/unpack.c && /tmp/unpack /pack.tmp %s' % remote, timeout)
    run_cmd(con, 'rm /pack.tmp', 30, echo=False)
    print('%s -> %s: %d files' % (local, remote, count))


# ---- Wi-Fi（ssh）----

SSH_KEY = os.environ.get('NOS_SSH_KEY', os.path.expanduser('~/.ssh/id_ecdsa_monocs'))


STOP_APP = os.environ.get('NOS_STOP_APP') == '1'   # --stop-app


def ssh_args(host, connect_timeout=10):
    # --stop-app: コンソールで動いているアプリ（top など）に待たされたら、止めてから動かす（sshd が Ctrl-\ を送る）
    stop = ['-o', 'SetEnv=MONOCS_STOP_APP=1'] if STOP_APP else []
    return ['ssh'] + stop + ['-o', 'BatchMode=yes', '-o', 'ConnectTimeout=%d' % connect_timeout,
            '-o', 'StrictHostKeyChecking=accept-new', '-o', 'ServerAliveInterval=15', '-o', 'LogLevel=ERROR',
            '-o', 'IdentitiesOnly=yes', '-i', SSH_KEY, 'monocs@' + host]


def ssh_cmd(host, line, timeout, stdin=None, capture=False):
    # シェルの 1 行を動かす。出力はそのまま表示する（capture なら返す）。戻り値は (終了の値, 出力)
    import subprocess
    r = subprocess.run(ssh_args(host) + [line], stdin=stdin if stdin is not None else subprocess.DEVNULL,
                       stdout=subprocess.PIPE if capture else None, timeout=timeout)
    return r.returncode, (r.stdout if capture else None)


def ssh_quote(s):
    # Monocs のシェルの '…'（中の ' は '\''）
    return "'" + s.replace("'", "'\\''") + "'"


def sftp_quote(p):
    # sftp の -b の行の中のパス（"…"。中の " と \ は \ を前に）
    return '"' + p.replace('\\', '\\\\').replace('"', '\\"') + '"'


def ssh_push(host, local, remote, data=None):
    # ふつうの sftp で送る（Monocs の sshd の sftp。services/sshd/sftp.c）。途中で切れても前のファイルが壊れないよう、
    # 名前.tmp に送ってから、置き換える名前の変更（posix-rename）で入れ替える。途中のフォルダは -mkdir で作る
    # （- は、すでにあっても誤りにしない）。data を渡したときは、それを一時ファイルにして送る（local は表示だけ）
    import subprocess
    import tempfile
    tmpfile = None
    if data is not None:
        tf = tempfile.NamedTemporaryFile(delete=False)
        tf.write(data)
        tf.close()
        tmpfile = src = tf.name
    else:
        src = local
    size = os.path.getsize(src)
    batch = []
    parts = [x for x in remote.split('/')[:-1] if x]
    for i in range(len(parts)):
        batch.append('-mkdir ' + sftp_quote('/' + '/'.join(parts[:i + 1])))
    batch.append('put %s %s' % (sftp_quote(src), sftp_quote(remote + '.tmp')))
    batch.append('rename %s %s' % (sftp_quote(remote + '.tmp'), sftp_quote(remote)))
    args = ssh_args(host)
    # ssh の引数を sftp の形に（sftp は宛先を最後に、-o はそのまま使える）
    sftp = ['sftp', '-q', '-b', '-'] + [x for x in args[1:-1]] + [args[-1]]
    t0 = time.time()
    try:
        r = subprocess.run(sftp, input='\n'.join(batch).encode(), stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, timeout=600)
    finally:
        if tmpfile:
            os.unlink(tmpfile)
    if r.returncode != 0:
        raise RuntimeError('push %s: sftp failed: %s' % (remote, r.stderr.decode(errors='replace').strip()))
    dt = time.time() - t0
    print('%s -> %s: %d bytes, %.1f s (%.0f KB/s, sftp)' % (local, remote, size, dt,
                                                           size / 1024 / max(dt, 1e-6)), flush=True)


def ssh_wait(host, timeout):
    # ssh がつながってシェルが答えるまで待つ
    import subprocess
    end = time.time() + timeout
    while True:
        try:
            r = subprocess.run(ssh_args(host, 5) + ['true'], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, timeout=20)
            if r.returncode == 0:
                return
        except subprocess.TimeoutExpired:
            pass
        if time.time() > end:
            raise TimeoutError('timeout waiting for ssh %s' % host)
        time.sleep(2)


def ssh_reboot(host, timeout):
    import subprocess
    # reboot は接続が切れたことを知らせずに止まるので、ssh の終わりは待たない（3 秒で打ち切る。SD カードへの
    # 書き出しと再起動はその間に始まる）
    t0 = time.time()
    p = subprocess.Popen(ssh_args(host) + ['reboot'], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    try:
        p.wait(3)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
    time.sleep(3)       # 古いカーネルが止まるまで（すぐつなぐと、止まる前のものにつながる）
    ssh_wait(host, timeout)
    print('booted (ssh after %.0f s)' % (time.time() - t0))


def ssh_poweroff(host):
    import subprocess
    p = subprocess.Popen(ssh_args(host) + ['poweroff'], stdin=subprocess.DEVNULL)
    try:
        p.wait(5)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
    # 止まったか: しばらく ssh がつながらないこと（再起動してしまったなら、また答える）
    try:
        ssh_wait(host, 25)
    except TimeoutError:
        print('halted（電源を切ってかまいません。入れ直すと起動します）')
        return 0
    raise RuntimeError('poweroff: 止まらずに、また答えています（再起動した）')


def ssh_kernel(host, img, timeout):
    # 新しいカーネルを試しに起動して（os/kernel/tryboot.c）、ssh が通ったら kernel8.img にする。だめなら、ファームウェアが
    # 前のカーネル（kernel8.img のまま）で起動し直すので、それを待って知らせる
    import subprocess
    rc, _ = ssh_cmd(host, 'test -f /boot/kernel8.img', 30)
    d = '/boot/' if rc == 0 else '/'
    # tryboot.txt: 今の config.txt の kernel= を試すカーネルに替え、試しの起動の印（コマンドライン）を足す
    # /usr/bin（2026-11-17 からのフォルダの構成）の cat があればそれを（移した直後の古いカーネルの PATH には /usr/bin がない）
    rc, conf = ssh_cmd(host, 'if test -f /usr/bin/cat; then /usr/bin/cat /config.txt; else cat /config.txt; fi', 30, capture=True)
    if rc != 0:
        raise RuntimeError('kernel: cannot read /config.txt')
    lines = [l for l in conf.decode().split('\n') if not l.startswith('kernel=') and not l.startswith('cmdline=')]
    lines += ['', '# nos.py kernel の試しの起動（reboot tryboot の 1 回だけ読まれる。os/kernel/tryboot.c）',
              'kernel=kernel8.trial.img', 'cmdline=tryboot-cmdline.txt', '']
    ssh_push(host, img, d + 'kernel8.trial.img')
    sym = os.path.splitext(img)[0] + '.sym'
    if os.path.exists(sym):
        ssh_push(host, sym, d + 'kernel8.trial.sym')
    ssh_push(host, '(tryboot.txt)', '/tryboot.txt', '\n'.join(lines).encode())
    # コマンドラインのファイルは、os_prefix が付くか資料でわからないので、両方に置く
    for where in sorted({d, '/'}):
        ssh_push(host, '(tryboot-cmdline.txt)', where + 'tryboot-cmdline.txt', b'monocs.tryboot\n')
    # ルートのものは隠し属性に（ls / に出さない。ファームウェアのファイルと同じ）
    ssh_cmd(host, 'fatattr +h /tryboot.txt /tryboot-cmdline.txt', 30)
    t0 = time.time()
    p = subprocess.Popen(ssh_args(host) + ['sync; reboot tryboot'], stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        p.wait(3)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
    time.sleep(3)
    ssh_wait(host, 200)
    rc, out = ssh_cmd(host, 'tryboot confirm', 30, capture=True)
    if rc == 0:
        promote = ('cp {d}kernel8.trial.img {d}kernel8.new && mv {d}kernel8.new {d}kernel8.img && '
                   'rm {d}kernel8.trial.img').format(d=d)
        if os.path.exists(sym):
            promote += ' && cp {d}kernel8.trial.sym {d}kernel8.sym && rm {d}kernel8.trial.sym'.format(d=d)
        rc, _ = ssh_cmd(host, promote + ' && sync', 120)
        if rc != 0:
            raise RuntimeError('kernel: confirmed, but could not copy kernel8.trial.img to kernel8.img')
        print('booted the new kernel and confirmed it (ssh after %.0f s); it is now %skernel8.img' % (time.time() - t0, d))
        return
    # 試しの起動ではない: 新しいカーネルが起動しなかった・ssh が通らなかったので、前のカーネルに戻った
    print('nos.py: the new kernel did not come up; the firmware went back to the previous kernel', file=sys.stderr)
    # 新しいカーネルの記録（前の起動の /var/log/dmesg.1。os/kernel/klog_file.c。固まる直前の 5 秒ほどは入らない）
    print('--- the new kernel\'s log (/var/log/dmesg.1) ---', file=sys.stderr)
    ssh_cmd(host, 'tail -30 /var/log/dmesg.1', 30)
    raise RuntimeError('kernel: new kernel failed (left as %skernel8.trial.img)' % d)


def wifi_main(a, host):
    if a.action == 'wait':
        ssh_wait(host, a.timeout)
        print('ready')
    elif a.action == 'cmd':
        return ssh_cmd(host, ' '.join(a.args), a.timeout)[0]
    elif a.action == 'log':
        return ssh_cmd(host, 'dmesg', a.timeout)[0]
    elif a.action == 'push':
        ssh_push(host, a.args[0], a.args[1])
    elif a.action == 'run':
        src = a.args[0]
        remote = '/tmp/' + os.path.basename(src)
        ssh_push(host, src, remote)
        return ssh_cmd(host, ' '.join(['run', remote] + a.args[1:]), a.timeout)[0]
    elif a.action == 'pushdir':
        pack, count = make_pack(a.args[0])
        ssh_push(host, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'unpack.c'), '/tmp/unpack.c')
        ssh_push(host, '(%d files)' % count, '/pack.tmp', bytes(pack))
        rc = ssh_cmd(host, 'cc -o /tmp/unpack /tmp/unpack.c && /tmp/unpack /pack.tmp %s; rm /pack.tmp' % ssh_quote(a.args[1]),
                     a.timeout)[0]
        print('%s -> %s: %d files' % (a.args[0], a.args[1], count))
        return rc
    elif a.action == 'kernel':
        ssh_kernel(host, a.args[0] if a.args else 'build/kslim/kernel8.img', a.timeout)
    elif a.action == 'reboot':
        ssh_reboot(host, a.timeout)
    elif a.action == 'poweroff':
        return ssh_poweroff(host)
    elif a.action == 'boot':
        ssh_wait(host, a.timeout)
        print('booted')


def pick_port(a):
    # 既定は USB。現れなければ（起動の直後は列挙が終わるまでない）少し待ち、それでもなければ UART
    if a.port:
        return a.port
    if a.uart:
        return UART_PORT
    end = time.time() + (15 if a.usb else 3)
    while not usb_port() and time.time() < end:
        time.sleep(0.1)
    if usb_port():
        return usb_port()
    if a.usb:
        raise TimeoutError('timeout waiting for %s' % USB_PORT)
    print('nos.py: USB のコンソールがないので UART（%s）を使います' % UART_PORT, file=sys.stderr)
    return UART_PORT


def main():
    ap = argparse.ArgumentParser(description='Monocs を USB（既定）か UART から操作する')
    ap.add_argument('--port', help='使う端末（既定は USB、なければ UART）')
    ap.add_argument('--usb', action='store_true', help='USB のコンソール（%s）だけを使う' % USB_PORT)
    ap.add_argument('--uart', action='store_true', help='UART（%s）を使う' % UART_PORT)
    ap.add_argument('--serial', action='store_true', help='ファイルを HTTP ではなくコンソールで送る')
    ap.add_argument('--baud', type=int, default=3000000)
    ap.add_argument('--timeout', type=int, default=600)
    ap.add_argument('--host', help='Wi-Fi で使う（Monocs のアドレス。ssh でつなぐ。既定は環境変数 NOS_HOST）')
    ap.add_argument('--stop-app', action='store_true',
                    help='Wi-Fi のとき、コンソールで動いているアプリに待たされたら止めてから動かす（NOS_STOP_APP=1）')
    ap.add_argument('--force', action='store_true',
                    help='実機が使われていても（アプリが動いている・telnet・ssh でつながっている）打ち込む')
    ap.add_argument('action', choices=['cmd', 'push', 'pushdir', 'kernel', 'run', 'reboot', 'wait',
                                       'boot', 'log', 'poweroff'])
    ap.add_argument('args', nargs='*')
    a = ap.parse_args()
    if a.serial:
        global use_http
        use_http = False
    host = a.host or (None if (a.uart or a.port) else os.environ.get('NOS_HOST'))
    if a.stop_app:
        global STOP_APP
        STOP_APP = True
    if host:
        return wifi_main(a, host)

    if a.action == 'boot':
        # 電源を入れ直したとき: 起動の表示を UART で見て、USB が使えるようになるまで待つ
        if not a.usb and os.path.exists(UART_PORT):
            mon = Console(a.port or UART_PORT, a.baud)
            show_boot(mon, a.timeout)
            mon.read_until(PROMPT, 30)
            os.close(mon.fd)
        if not a.uart and not a.port:
            end = time.time() + 15
            while not usb_port() and time.time() < end:
                time.sleep(0.1)
            print('booted' + ('' if usb_port() else '（USB のコンソールはない）'))
        else:
            print('booted')
        return

    if a.action == 'log' and show_log():
        return
    check_idle(a.force)
    con = Console(pick_port(a), a.baud)
    if a.action == 'wait':
        # プロンプトが来なければ、数秒おきに Enter を送って出させる（起動の表示はもう流れたかもしれない）
        end = time.time() + a.timeout
        while True:
            try:
                con.read_until(PROMPT, 3)
                break
            except TimeoutError:
                if time.time() > end:
                    raise
                con.write(b'\r')
        print('ready')
        return
    sync(con)
    if a.action == 'log':
        run_cmd(con, 'dmesg', a.timeout)
    elif a.action == 'cmd':
        run_cmd(con, ' '.join(a.args), a.timeout)
        return last_status()
    elif a.action == 'push':
        push(con, a.args[0], a.args[1])
    elif a.action == 'kernel':
        img = a.args[0] if a.args else 'build/kslim/kernel8.img'
        # 置き場所: /boot/kernel8.img（config.txt の os_prefix=boot/。2026-11-13 から）。/boot にカーネルが
        # なければ、前の SD カードなので /kernel8.img
        out = run_cmd(con, 'ls /boot/kernel8.img', 10, echo=False)
        dest = '/boot/' if b'No such' not in out and b'cannot' not in out else '/'
        push(con, img, dest + 'kernel8.img')
        # カーネルの関数名の表（make が隣に作る。シェルの perf record が使う）
        sym = os.path.splitext(img)[0] + '.sym'
        if os.path.exists(sym):
            push(con, sym, dest + 'kernel8.sym')
        reboot(con)
    elif a.action == 'run':
        src = a.args[0]
        remote = '/tmp/' + os.path.basename(src)
        run_cmd(con, 'mkdir /tmp', 10, echo=False)
        push(con, src, remote)
        run_cmd(con, ' '.join(['run', remote] + a.args[1:]), a.timeout)
        return last_status()
    elif a.action == 'reboot':
        reboot(con)
    elif a.action == 'poweroff':
        con.write(b' poweroff\r')
        out = con.read_until(b'poweroff: SD', 30) + con.read_until(b'\n', 5)
        print(out.decode(errors='replace').replace('\r', '').strip().splitlines()[-1])
    elif a.action == 'pushdir':
        pushdir(con, a.args[0], a.args[1], a.timeout)


if __name__ == '__main__':
    # 出力を head などで途中まで読んで閉じられたら、黙って終わる（Linux のコマンドと同じ）
    import signal
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    try:
        sys.exit(main() or 0)
    except (TimeoutError, RuntimeError) as e:
        print('nos.py: %s' % e, file=sys.stderr)
        sys.exit(1)
