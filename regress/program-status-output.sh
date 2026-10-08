#!/bin/sh
set -eu
DIR=$(mktemp -d /tmp/tmux-status-output-XXXXXX)
[ -n "${TEST_TMUX:-}" ] || TEST_TMUX=$(cd .. && pwd)/tmux
export TEST_TMUX DIR
trap '"$TEST_TMUX" -S "$DIR/socket" kill-server 2>/dev/null || :; rm -rf "$DIR"' 0 1 2 15
python3 -u - <<'PY'
import base64
import json
import os
import select
import shlex
import signal
import subprocess
import sys
import time

signal.alarm(30)
root = os.environ['DIR']
server = [os.environ['TEST_TMUX'], '-S', root + '/socket', '-f', '/dev/null']
fifo = root + '/input'
os.mkfifo(fifo)
# Filler output on one thread, reports on another, one lock so a report is
# never split by filler.
code = root + '/pane.py'
with open(code, 'w') as out:
    out.write("""import os, sys, threading, time
lock = threading.Lock()
stop = threading.Event()
def output():
    while not stop.is_set():
        with lock:
            os.write(1, b'x' * 512 + b'\\r\\n')
        time.sleep(.002)
t = threading.Thread(target=output)
t.start()
for line in open(sys.argv[1]):
    if line.strip() == 'stop':
        break
    with lock:
        os.write(1, b'\\x1b]7501;' + line.strip().encode() + b'\\x07')
stop.set()
t.join()
time.sleep(30)
""")
command = ' '.join(map(shlex.quote, [sys.executable, '-u', code, fifo]))
pane = subprocess.check_output(server + ['new-session', '-d', '-s', 'stress',
                               '-P', '-F', '#{pane_id}', command], timeout=5).strip()
writer = open(fifo, 'w', buffering=1)
client = subprocess.Popen(server + ['-C', 'attach-session', '-t', 'stress'],
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)
os.set_blocking(client.stdout.fileno(), False)

def read_until(needle, timeout):
    end = time.monotonic() + timeout
    data = b''
    while time.monotonic() < end:
        if select.select([client.stdout], [], [], .05)[0]:
            chunk = os.read(client.stdout.fileno(), 1000000)
            assert chunk, ('client exited', data)
            data += chunk
            if needle in data:
                return data
    raise AssertionError(('status/output starved', needle, data[-2000:]))

read_until(b'%output ' + pane + b' ', 5)
# The largest legal set: 64 records, each with a full title and msg.
title = base64.b64encode(b't' * 192).decode()
msg = base64.b64encode(b'm' * 2048).decode()
started = time.monotonic()
for i in range(64):
    writer.write('state=done:app=stress:id=r%02d:title=%s:msg=%s\n' % (i, title, msg))
prefix = b'%program-status ' + pane + b' '
buffer = b''
payload = None
while payload is None:
    assert time.monotonic() - started < 10, ('status/output starved', buffer[-2000:])
    if select.select([client.stdout], [], [], .05)[0]:
        chunk = os.read(client.stdout.fileno(), 1000000)
        assert chunk, ('client exited', buffer[-2000:])
        buffer += chunk
    *lines, buffer = buffer.split(b'\n')
    for line in lines:
        if line.startswith(prefix):
            serial, text = line.split(b' ', 3)[2:]
            candidate = json.loads(text)
            assert int(serial) == candidate['serial']
            if len(candidate['records']) == 64:
                payload, size = candidate, len(line)
assert all(r['msg'] == msg and r['title'] == title for r in payload['records'])
assert size > 64 * 2732, size
elapsed = time.monotonic() - started
read_until(b'%output ' + pane + b' ', 2)
writer.write('stop\n')
writer.close()
client.stdin.write(b'detach-client\n'); client.stdin.flush()
os.set_blocking(client.stdout.fileno(), True)
client.communicate(timeout=5)
print('same-session sustained output/status: PASS (64 x 2048-byte msgs, %d-byte line in %.3fs, output continued)' % (size, elapsed))
PY
