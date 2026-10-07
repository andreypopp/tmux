#!/bin/sh
set -eu
DIR=$(mktemp -d /tmp/tmux-status-output-XXXXXX)
[ -n "${TEST_TMUX:-}" ] || TEST_TMUX=$(cd .. && pwd)/tmux
export TEST_TMUX DIR
trap '"$TEST_TMUX" -S "$DIR/socket" kill-server 2>/dev/null || :; rm -rf "$DIR"' 0 1 2 15
python3 -u - <<'PY'
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
code = "import os,sys,threading,time; f=open(sys.argv[1]); " \
       "stop=threading.Event(); " \
       "exec('def output():\\n while not stop.is_set():\\n" \
       "  os.write(1,b\"x\"*512+b\"\\\\r\\\\n\")\\n  time.sleep(.002)'); " \
       "t=threading.Thread(target=output); t.start(); " \
       "exec('for line in f:\\n if line.strip()==\"stop\": break\\n" \
       " os.write(1,b\"\\\\x1b]7501;state=done:app=stress\\\\x07\")'); " \
       "stop.set(); t.join(); time.sleep(30)"
command = ' '.join(map(shlex.quote, [sys.executable, '-u', '-c', code, fifo]))
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
started = time.monotonic()
writer.write('report\n')
notification = read_until(b'%program-status ' + pane + b' ', 5)
# Complete the notification line if the pipe split it at the prefix.
while not notification.endswith(b'\n'):
    notification += read_until(b'\n', 1)
line = next(line for line in notification.splitlines()
            if line.startswith(b'%program-status ' + pane + b' '))
payload = json.loads(line.split(b' ', 3)[3])
assert payload['records'] == [{'id': '', 'state': 'done', 'app': 'stress'}]
assert int(line.split(b' ', 3)[2]) == payload['serial']
elapsed = time.monotonic() - started
read_until(b'%output ' + pane + b' ', 2)
writer.write('stop\n')
writer.close()
client.stdin.write(b'detach-client\n'); client.stdin.flush()
os.set_blocking(client.stdout.fileno(), True)
client.communicate(timeout=5)
print('same-session sustained output/status: PASS (%.3fs, output continued)' % elapsed)
PY
