#!/bin/sh
set -eu
DIR=$(mktemp -d /tmp/tmux-input-discard-XXXXXX)
[ -n "${TEST_TMUX:-}" ] || TEST_TMUX=$(cd .. && pwd)/tmux
export TEST_TMUX DIR
trap '"$TEST_TMUX" -S "$DIR/socket" kill-server 2>/dev/null || :; rm -rf "$DIR"' 0 1 2 15
python3 -u - <<'PY'
import base64
import os
import shlex
import signal
import subprocess
import sys
import time

signal.alarm(90)
root = os.environ['DIR']
server = [os.environ['TEST_TMUX'], '-S', root + '/socket', '-f', '/dev/null']

def cmd(*args):
    return subprocess.check_output(server + list(args), timeout=5)

def wait(fn, name):
    end = time.monotonic() + 5
    while time.monotonic() < end:
        if fn():
            return
        time.sleep(.01)
    raise AssertionError('timeout: ' + name)

fifo = root + '/input'
os.mkfifo(fifo)
code = "import base64,os,sys; f=open(sys.argv[1],'rb',buffering=0); n=0; " \
       "exec('while True:\\n b=f.readline()\\n if not b: break\\n" \
       " os.write(1,base64.b64decode(b))\\n n+=1\\n" \
       " open(sys.argv[2]+str(n),\"w\").close()')"
command = ' '.join(map(shlex.quote, [sys.executable, '-u', '-c', code, fifo, root + '/done']))
pane = cmd('new-session', '-d', '-P', '-F', '#{pane_id}', command).decode().strip()
writer = open(fifo, 'wb', buffering=0)
number = 0

def send(data):
    global number
    number += 1
    writer.write(base64.b64encode(data) + b'\n')
    wait(lambda: os.path.exists(root + '/done' + str(number)), 'PTY write')
    time.sleep(.1)

for name, prefix, terminator, cap in [
    ('OSC', b'\x1b]999;', b'\x07', 1048608),
    ('DCS', b'\x1bPq', b'\x1b\\', 1048608),
    ('APC', b'\x1b_', b'\x1b\\', 1048608),
    ('CSI', b'\x1b[', b'm', 128),
]:
    data = b'1' * (200000 if name == 'CSI' else 2000000)
    send(prefix + data)
    pending = cmd('capture-pane', '-pP', '-t', pane)
    assert 0 < len(pending) <= cap, (name, 'pending length', len(pending), cap)
    tag = 'recovered-' + name
    send(terminator + b'\x1b[2J\x1b[HRECOVERED-' + name.encode() +
         b'\x1b]2;' + tag.encode() + b'\x07')
    wait(lambda: cmd('display-message', '-p', '-t', pane, '#{pane_title}').strip()
         == tag.encode(), name + ' resync')
    assert b'RECOVERED-' + name.encode() in cmd('capture-pane', '-p', '-t', pane)
    assert cmd('capture-pane', '-pP', '-t', pane).strip() == b''
    print('discard/pending/resync ' + name + ': PASS')
writer.close()
PY
