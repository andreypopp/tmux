"""Helpers for image tests using an attached client on a PTY."""

import _posixshmem as shm
import base64
import contextlib
import colorsys
import errno
import fcntl
import itertools
import mmap
import os
import re
import select
import signal
import struct
import subprocess
import sys
import termios
import tempfile
import time
import zlib


class SharedMemory:
    def __init__(self):
        self.names = []

    def create(self, data, name=None):
        if name is None:
            name = '/kis%d-%d' % (os.getpid(), len(self.names))
        fd = shm.shm_open(name, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        try:
            os.ftruncate(fd, len(data))
            with mmap.mmap(fd, len(data)) as view:
                view[:] = data
        finally:
            os.close(fd)
        self.names.append(name)
        return name

    def close(self):
        for name in self.names:
            try:
                shm.shm_unlink(name)
            except FileNotFoundError:
                pass


def shm_exists(name):
    try:
        fd = shm.shm_open(name, os.O_RDONLY, 0)
    except FileNotFoundError:
        return False
    os.close(fd)
    return True


def shm_contents(name, size=64):
    fd = shm.shm_open(name, os.O_RDONLY, 0)
    try:
        with mmap.mmap(fd, os.fstat(fd).st_size, access=mmap.ACCESS_READ) as view:
            return view[:size]
    finally:
        os.close(fd)


def unescape(data):
    return re.sub(rb'\\([0-7]{3})', lambda m: bytes([int(m[1], 8)]), data)


class Control:
    def __init__(self, test, flags):
        self.process = subprocess.Popen(test.server + ['-C', 'attach-session',
            '-t', 'image', '-f', flags], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        os.set_blocking(self.process.stdout.fileno(), False)
        self.buffer = b''
        self.output = b''
        self.exchange(None)

    def exchange(self, command):
        if command is not None:
            self.process.stdin.write(command.encode() + b'\n')
            self.process.stdin.flush()
        reply = []
        begun = False
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if b'\n' not in self.buffer:
                if not select.select([self.process.stdout], [], [], 0.1)[0]:
                    continue
                data = os.read(self.process.stdout.fileno(), 65536)
                assert data, 'control EOF'
                self.buffer += data
                continue
            line, self.buffer = self.buffer.split(b'\n', 1)
            if line.startswith(b'%output '):
                self.output += unescape(line.split(b' ', 2)[2])
            elif line.startswith(b'%begin '):
                begun = True
            elif begun and line.startswith((b'%end ', b'%error ')):
                assert line.startswith(b'%end '), reply
                return b'\n'.join(reply) + (b'\n' if reply else b'')
            elif begun:
                reply.append(line)
        raise AssertionError('control timeout')

    def sync(self):
        self.exchange('display-message -p barrier')
        result, self.output = self.output, b''
        return result

    def close(self):
        self.process.stdin.close()
        deadline = time.monotonic() + 10
        while self.process.poll() is None and time.monotonic() < deadline:
            if select.select([self.process.stdout], [], [], 0.05)[0]:
                try:
                    os.read(self.process.stdout.fileno(), 65536)
                except BlockingIOError:
                    pass
        assert self.process.wait(timeout=1) == 0


class Client:
    """An attached client and the bytes written to its terminal."""

    def __init__(self, pid, fd):
        self.pid = pid
        self.fd = fd
        self.data = bytearray()

    def read(self, duration=0.1):
        """Read terminal output for a bounded time."""
        end = time.monotonic() + duration
        while time.monotonic() < end:
            if not select.select([self.fd], [], [], 0.01)[0]:
                continue
            try:
                data = os.read(self.fd, 65536)
            except OSError as e:
                if e.errno == errno.EIO:
                    raise RuntimeError("attached client exited") from e
                raise
            if not data:
                raise RuntimeError("attached client exited")
            self.data.extend(data)

    def clear(self):
        """Finish reading earlier output before starting a new check."""
        self.read()
        self.data.clear()

    def images(self):
        """Decode complete SIXEL sequences from the terminal output."""
        images = []
        for params, body in re.findall(
                rb"\033P([0-9;]*)q(.*?)\033\\", self.data, re.S):
            params = params.split(b";")
            p2 = int(params[1] or b"0") if len(params) > 1 else 0
            images.append(decode_sixel(body, p2))
        return images


class ImageTest:
    """A server with one pane which copies input directly to its output."""

    def __init__(self, tmux, sx=40, sy=16):
        self.directory = tempfile.TemporaryDirectory(prefix='is-', dir='/tmp')
        self.server = [tmux, '-S', self.directory.name + '/socket', '-f/dev/null']
        self.sx = sx
        self.sy = sy
        self.clients = []
        self.marker = 0

    def run(self, *args, check=True, input=None):
        """Run a command on this server."""
        p = subprocess.run(self.server + list(args), capture_output=True,
            input=input, timeout=10)
        if check and p.returncode != 0:
            raise RuntimeError("%r: %s" % (args, p.stderr.decode()))
        return p.stdout.decode().strip()

    def wait(self, predicate):
        """Read client output while waiting for a server state change."""
        end = time.monotonic() + 10
        while True:
            for client in self.clients:
                client.read(0.01)
            if predicate():
                return
            if time.monotonic() >= end:
                raise RuntimeError("timed out waiting for server state")
            time.sleep(0.01)

    def start(self):
        """Start the pane and skip when the server has no image support."""
        self.run("new-session", "-d", "-s", "image", "-x", str(self.sx),
            "-y", str(self.sy), "stty raw -echo; "
            "printf '\033]2;READY\007'; exec cat")
        support = self.run("display-message", "-p", "#{image_support}")
        if support == "0":
            sys.exit(101)
        assert support == "1", ("unexpected image_support", support)
        self.run("set-option", "-g", "status", "off")
        self.wait(lambda: self.run("display-message", "-p",
            "#{pane_title}") == "READY")

    def record(self):
        self.replies = self.directory.name + '/replies'
        self.run('respawn-pane', '-k', "stty raw -echo; printf '\033]2;recorder\007'; exec cat > " + self.replies)
        self.wait(lambda: self.run('display-message', '-p', '#{pane_title}') == 'recorder')
        self.pane = self.run('display-message', '-p', '#{pane_id}')
        self.recorder = os.open(self.run('display-message', '-p', '#{pane_tty}'), os.O_WRONLY)
        def send(data):
            while data:
                data = data[os.write(self.recorder, data):]
        self.send = send

    def capture(self, *extra, check=True):
        result = subprocess.run(self.server + ['capture-pane', '-K', '-p', '-t', self.pane] + list(extra),
            capture_output=True, timeout=10, check=check)
        return result.stdout if check else result

    def frames(self, data=None, objects=None):
        result = []
        for keys, value in re.findall(rb'\033_G([^;]*);(.*?)\033\\', self.capture() if data is None else data):
            if b'a=t,' in keys or b'a=f,' in keys:
                result.append([keys, b''])
            if b't=s,' in keys:
                name = base64.b64decode(value).decode()
                objects.names.append(name)
                result[-1][1] = shm_contents(name, int(re.search(rb'S=(\d+)', keys)[1]))
            else:
                result[-1][1] += base64.b64decode(value)
        return result

    def recorded(self, data, expected):
        before = os.path.getsize(self.replies)
        self.emit(data)
        self.wait(lambda: os.path.getsize(self.replies) >= before + len(expected))
        with open(self.replies, 'rb') as f:
            f.seek(before)
            actual = f.read()
        assert actual == expected, (actual, expected)

    def reply(self, keys, data, expected):
        self.recorded(kitty(keys + ',q=0', data), expected)

    def detach(self, client):
        self.run('detach-client', '-t', client.name)
        self.clients.remove(client)
        os.close(client.fd)
        self.wait(lambda: os.waitpid(client.pid, os.WNOHANG)[0] != 0)

    def attach(self, graphical=True, utf8=True, cw=8, ch=16, term=None):
        """Attach a client with a known cell pixel size and image feature."""
        if term is None:
            term = "xterm-256color" if graphical else "screen-256color"
        features = term + (":sixel" if graphical else ":sixel@")
        self.run("set-option", "-as", "terminal-features", features)
        pid, fd = os.forkpty()
        if pid == 0:
            try:
                os.environ.pop("TMUX", None)
                os.environ["TERM"] = term
                os.environ["LC_ALL"] = "C.UTF-8" if utf8 else "C"
                fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH",
                    self.sy, self.sx, self.sx * cw, self.sy * ch))
                args = self.server + (["-u"] if utf8 else [])
                os.execv(self.server[0], args +
                    ["attach-session", "-t", "image"])
            finally:
                os._exit(1)
        client = Client(pid, fd)
        self.clients.append(client)
        self.wait(lambda: any(line.startswith(str(pid) + ":") for line in
            self.run("list-clients", "-F", "#{client_pid}:").splitlines()))
        # Answer DA so the startup timeout cannot schedule a later redraw.
        os.write(fd, b"\033[?65;4c" if graphical else b"\033[?65c")
        self.wait(lambda: self.client_size(client) ==
            "%dx%d:%dx%d" % (self.sx, self.sy, cw, ch))
        if len(self.clients) == 1 and cw != 0 and ch != 0:
            self.wait(lambda: self.run("display-message", "-p",
                "#{window_cell_width}x#{window_cell_height}") ==
                "%dx%d" % (cw, ch))
        client.name = next(line.split(":", 1)[1] for line in
            self.run("list-clients", "-F",
                "#{client_pid}:#{client_name}").splitlines()
            if line.startswith(str(pid) + ":"))
        client.read(0.2)
        client.clear()
        return client

    def client_size(self, client):
        """Get the character and cell pixel size of one client."""
        prefix = str(client.pid) + ":"
        return next((line[len(prefix):] for line in self.run("list-clients",
            "-F", "#{client_pid}:#{client_width}x#{client_height}:"
            "#{client_cell_width}x#{client_cell_height}").splitlines()
            if line.startswith(prefix)), None)

    def send(self, data):
        """Send literal bytes to the pane."""
        if len(data) > 512:
            self.run("load-buffer", "-b", "image", "-", input=data)
            self.run("paste-buffer", "-b", "image", "-S", "-r", "-d")
        else:
            self.run("send-keys", "-H", *["%02x" % ch for ch in data])

    def emit(self, data):
        """Send pane output and wait until its trailing title is parsed."""
        for client in self.clients:
            client.clear()
        self.marker += 1
        title = "image-%d" % self.marker
        self.send(data + b"\033]2;" + title.encode() + b"\007")
        self.wait(lambda: self.run("display-message", "-p",
            "#{pane_title}") == title)
        for client in self.clients:
            client.read(0.2)

    def redraw(self):
        """Request a full redraw and collect its terminal output."""
        for client in self.clients:
            client.clear()
        for client in self.clients:
            self.run("refresh-client", "-t", client.name)
        for client in self.clients:
            client.read(0.2)

    def resize(self, client, sx, sy, cw=8, ch=16):
        """Resize one client's terminal and wait for the window size."""
        client.clear()
        fcntl.ioctl(client.fd, termios.TIOCSWINSZ,
            struct.pack("HHHH", sy, sx, sx * cw, sy * ch))
        self.wait(lambda: self.client_size(client) ==
            "%dx%d:%dx%d" % (sx, sy, cw, ch))
        self.wait(lambda: self.run("display-message", "-p",
            "#{window_width}x#{window_height}") == "%dx%d" % (sx, sy))
        client.read(0.2)

    def close(self):
        """Stop the server and reap every attached client."""
        subprocess.run(["/bin/sh", "-c",
            '. ./regress-common.inc; stop_server "$@"', "image-test",
            *self.server], check=True, timeout=15)
        for client in self.clients:
            os.close(client.fd)
            if os.waitpid(client.pid, os.WNOHANG) == (0, 0):
                os.kill(client.pid, signal.SIGTERM)
                os.waitpid(client.pid, 0)
        if hasattr(self, 'recorder'):
            os.close(self.recorder)
        self.directory.cleanup()


@contextlib.contextmanager
def image_test(tmux, sx=40, sy=16):
    """Clean up the server even when a test fails or skips."""
    test = ImageTest(tmux, sx, sy)
    try:
        test.start()
        yield test
    finally:
        test.close()


def token(data):
    return re.search(rb'^more (.*)$', data, re.M)[1].decode()


def kitty(control, data=b''):
    """Build a Kitty graphics APC with replies suppressed."""
    return b'\033_Gq=2,' + control.encode() + b';' + \
        base64.b64encode(data) + b'\033\\'


def png(w, h, rows, colour_type=6, chunks=()):
    """Build an eight-bit PNG from unfiltered rows and optional chunks."""
    assert len(rows) == h, (len(rows), h)
    parts = [(b'IHDR', struct.pack('>2I5B', w, h, 8, colour_type, 0, 0, 0))]
    parts.extend(chunks)
    parts.append((b'IDAT', zlib.compress(b''.join(b'\0' + row for row in rows))))
    parts.append((b'IEND', b''))
    out = b'\x89PNG\r\n\x1a\n'
    for kind, data in parts:
        out += struct.pack('>I', len(data)) + kind + data + \
            struct.pack('>I', zlib.crc32(kind + data))
    return out


def sixel(w, h, palette, pixels, p2=1):
    """Build SIXEL input from a palette and row-major palette indexes."""
    assert len(pixels) == w * h
    out = ['\033P9;%dq"1;1;%d;%d' % (p2, w, h)]
    for c, rgb in enumerate(palette):
        out.append("#%d;2;%d;%d;%d" % (c, *rgb))
    for y in range(0, h, 6):
        planes = []
        for c in range(len(palette)):
            columns = []
            for x in range(w):
                bits = sum(1 << bit for bit in range(min(6, h - y))
                    if pixels[(y + bit) * w + x] == c)
                columns.append(chr(0x3f + bits))
            if all(ch == "?" for ch in columns):
                continue
            plane = "#%d" % c
            for ch, run in itertools.groupby(columns):
                n = len(list(run))
                plane += "!%d%s" % (n, ch) if n > 3 else ch * n
            planes.append(plane)
        out.append("$".join(planes))
        if y + 6 < h:
            out.append("-")
    out.append("\033\\")
    return "".join(out).encode()


def decode_sixel(body, p2=1):
    """Read RGB output pixels without using tmux's SIXEL decoder."""
    attributes = re.match(rb'"1;1;(\d+);(\d+)', body)
    assert attributes is not None, ("missing raster attributes", body[:80])
    w, h = map(int, attributes.groups())
    pixels = [None if p2 == 1 else (0, 0, 0)] * (w * h)
    palette = {}
    c = x = y = 0
    pos = attributes.end()
    while pos < len(body):
        ch = body[pos]
        pos += 1
        if ch == ord("#"):
            match = re.match(rb"(\d+)(?:;([12]);(\d+);(\d+);(\d+))?", body[pos:])
            assert match is not None, body[pos:pos + 80]
            c = int(match[1])
            if match[2] is not None:
                values = tuple(int(match[i]) for i in range(3, 6))
                if match[2] == b"1":
                    hue, lightness, saturation = values
                    rgb = colorsys.hls_to_rgb(((hue + 240) % 360) / 360,
                        lightness / 100, saturation / 100)
                    palette[c] = tuple(int(v * 255 + .5) for v in rgb)
                else:
                    assert all(v <= 100 for v in values), values
                    palette[c] = tuple((v * 255 + 50) // 100 for v in values)
            pos += match.end()
            continue
        if ch == ord("$"):
            x = 0
            continue
        if ch == ord("-"):
            x = 0
            y += 6
            continue
        n = 1
        if ch == ord("!"):
            match = re.match(rb"(\d+)(.)", body[pos:], re.S)
            assert match is not None
            n = int(match[1])
            ch = match[2][0]
            pos += match.end()
        assert 0x3f <= ch <= 0x7e, ch
        for bit in range(6):
            if not (ch - 0x3f) & (1 << bit):
                continue
            assert y + bit < h and x + n <= w, (x, y, n, bit, w, h)
            assert c in palette, ("undefined output colour", c)
            for xx in range(x, x + n):
                pixels[(y + bit) * w + xx] = palette[c]
        x += n
    return w, h, pixels


def check_image(image, w, h, expected):
    """Compare decoded output with expected RGB pixels and transparency."""
    assert image[:2] == (w, h), (image[:2], (w, h))
    assert len(expected) == w * h
    for i, (actual, wanted) in enumerate(zip(image[2], expected)):
        if wanted is None:
            assert actual is None, (i % w, i // w, actual, wanted)
        else:
            assert actual is not None, (i % w, i // w, actual, wanted)
            assert max(abs(a - b) for a, b in zip(actual, wanted)) <= 3, (
                i % w, i // w, actual, wanted)
