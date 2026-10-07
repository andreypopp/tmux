#!/bin/sh
set -eu
DIR=$(mktemp -d /tmp/tmux-status-control-XXXXXX)
[ -n "${TEST_TMUX:-}" ] || TEST_TMUX=$(cd .. && pwd)/tmux
export TEST_TMUX DIR
trap '"$TEST_TMUX" -S "$DIR/socket" kill-server 2>/dev/null || :; rm -rf "$DIR"' 0 1 2 15
python3 -u - "${1:-all}" <<'PY'
import json
import os
import select
import shlex
import signal
import subprocess
import sys
import time

signal.alarm(90)
mode = sys.argv[1]
root = os.environ['DIR']
tmux = os.environ['TEST_TMUX']
server = [tmux, '-S', root + '/socket', '-f', '/dev/null']
clients = []

def cmd(*args):
    return subprocess.check_output(server + list(args), text=True, timeout=5).strip()

def wait(fn, name):
    end = time.monotonic() + 4
    while time.monotonic() < end:
        if fn():
            return
        time.sleep(.01)
    raise AssertionError('timeout: ' + name)

fifo = root + '/input'
os.mkfifo(fifo)
code = "import os,sys; f=open(sys.argv[1],'rb',buffering=0); " \
       "exec('while True:\\n b=f.read(4096)\\n if not b: break\\n os.write(1,b)')"
command = ' '.join(map(shlex.quote, [sys.executable, '-u', '-c', code, fifo]))
pane = cmd('new-session', '-d', '-s', 'source', '-P', '-F', '#{pane_id}', command)
window = cmd('display-message', '-p', '-t', pane, '#{window_id}')
writer = os.open(fifo, os.O_WRONLY)
cmd('new-session', '-d', '-s', 'observer', 'exec sleep 90')
marker = 0

def send(data, settle=True):
    global marker
    marker += 1
    tag = 'marker-%d' % marker
    os.write(writer, data + b'\x1b]2;' + tag.encode() + b'\x07')
    wait(lambda: cmd('display-message', '-p', '-t', pane, '#{pane_title}') == tag, tag)
    if settle:
        time.sleep(.13)

def snapshot():
    return json.loads(cmd('display-message', '-p', '-t', pane, '#{pane_program_status}'))

def control(session):
    p = subprocess.Popen(server + ['-C', 'attach-session', '-t', session],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)
    os.set_blocking(p.stdout.fileno(), False)
    clients.append(p)
    drain(p, .1)
    return p

def drain(p, duration=.1):
    data = b''
    end = time.monotonic() + duration
    while time.monotonic() < end:
        if select.select([p.stdout], [], [], max(0, end-time.monotonic()))[0]:
            chunk = os.read(p.stdout.fileno(), 1000000)
            if not chunk:
                break
            data += chunk
    return data

def instruction(p, text):
    p.stdin.write(text.encode() + b'\n')
    p.stdin.flush()

def statuses(data):
    return [json.loads(line.split(b' ', 3)[3]) for line in data.splitlines()
            if line.startswith(b'%program-status ' + pane.encode() + b' ')]

if mode in ['link', 'all']:
    observer = control('observer')
    send(b'\x1b]7501;state=done\x07' + b'x' * 2000)
    assert statuses(drain(observer))[-1] == snapshot()
    cmd('link-window', '-s', window, '-t', 'observer:1')
    os.write(writer, b'END')
    output = drain(observer, .4)
    assert b'%exit server exited unexpectedly' not in output, output
    assert b'%output ' + pane.encode() + b' END' in output, output
    cmd('has-session', '-t', 'source')
    cmd('unlink-window', '-t', 'observer:1')
    print('status-only link/output: PASS')

if mode in ['reset', 'pause', 'off', 'all']:
    same = control('source')
    peer = control('source')
    instruction(peer, 'refresh-client -f no-output')
    drain(peer)
    if mode in ['reset', 'all']:
        # A settled report followed by an immediate respawn must deliver the empty
        # emission even though output offsets are reset in the same command.
        send(b'\x1b]7501;state=done\x07')
        assert statuses(drain(same))[-1] == snapshot()
        cmd('respawn-pane', '-k', '-t', pane, 'exec sleep 90')
        os.close(writer)
        time.sleep(.15)
        clear = snapshot()
        assert clear['records'] == []
        received = statuses(drain(same, .3))
        assert clear in received, ('respawn lost clear', clear, received)
        cmd('respawn-pane', '-k', '-t', pane, command)
        writer = os.open(fifo, os.O_WRONLY)
    for action, resume in [('pause', 'continue'), ('off', 'on')]:
        if mode in ['pause', 'off'] and action != mode:
            continue
        send(b'\x1b]7501;state=done\x07')
        drain(same); drain(peer)
        instruction(same, 'refresh-client -A ' + pane + ':' + action)
        drain(same)
        # New reports must flow with this client's output paused/off.
        send(b'\x1b]7501;state=done\x07')
        assert snapshot() in statuses(drain(same, .3))
        send(b'\x1b]7501;state=clear\x07')
        assert snapshot() in statuses(drain(same, .3))
        send(b'\x1b]7501;state=done\x07')
        drain(same)
        # Respawn emits the clear and resets output in one synchronous command:
        # the clear is necessarily still in its unsent slot during the reset.
        cmd('respawn-pane', '-k', '-t', pane, 'exec sleep 90')
        os.close(writer)
        time.sleep(.15)
        clear = snapshot()
        assert clear['records'] == []
        received = statuses(drain(same, .3))
        assert clear in received, (action + ' respawn lost clear', clear, received)
        cmd('respawn-pane', '-k', '-t', pane, command)
        writer = os.open(fifo, os.O_WRONLY)
        instruction(same, 'refresh-client -A ' + pane + ':' + resume)
        drain(same)
    print('same-session respawn/pause/off clears: PASS')

if mode in ['memory', 'all']:
    # No terminator: capture's pending bytes expose since_ground retention.
    os.write(writer, b'\x1b]7501;state=working:future=' + b'x' * 100000)
    time.sleep(.2)
    pending = cmd('capture-pane', '-pP', '-t', pane)
    assert len(pending) <= 4096, ('discarded pending bytes', len(pending))
    send(b'\x07')
    print('discarded unterminated OSC pending buffer: PASS')

# Closing the writer ends the pane and its session, exiting attached clients.
for p in clients:
    instruction(p, 'detach-client')
    os.set_blocking(p.stdout.fileno(), True)
    p.communicate(timeout=5)
os.close(writer)
PY
