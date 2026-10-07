#!/bin/sh

set -eu
DIR=$(mktemp -d /tmp/tmux-program-status-XXXXXX)
[ -n "${TEST_TMUX:-}" ] || TEST_TMUX=$(cd .. && pwd)/tmux
export TEST_TMUX DIR
trap '"$TEST_TMUX" -S "$DIR/socket" kill-server 2>/dev/null || :; rm -rf "$DIR"' 0 1 2 15

python3 -u - <<'PY'
import base64
import json
import os
import select
import signal
import shlex
import subprocess
import sys
import time

signal.alarm(120)
root = os.environ['DIR']
tmux = os.environ['TEST_TMUX']
socket = root + '/socket'
clients = []

def cmd(*args):
    return subprocess.check_output([tmux, '-S', socket, '-f', '/dev/null',
                                    *args], text=True, timeout=5).strip()

def wait(fn, name, timeout=4):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        value = fn()
        if value:
            return value
        time.sleep(.01)
    raise AssertionError('timeout: ' + name)

fifo = root + '/input'
os.mkfifo(fifo)
writer_code = "import os,sys; f=open(sys.argv[1],'rb',buffering=0); " \
              "exec('while True:\\n b=f.read(4096)\\n if not b: break\\n os.write(1,b)')"
command = ' '.join(map(shlex.quote, [sys.executable, '-u', '-c', writer_code, fifo]))
pane = cmd('new-session', '-d', '-s', 'status', '-P', '-F', '#{pane_id}', command)
cmd('set-option', '-g', 'remain-on-exit', 'on')
writer = os.open(fifo, os.O_WRONLY)
marker = 0

def snapshot():
    return json.loads(cmd('display-message', '-p', '-t', pane,
                          '#{pane_program_status}'))

def send(data, settle=True):
    global marker
    marker += 1
    tag = 'marker-' + str(marker)
    os.write(writer, data + b'\x1b]2;' + tag.encode() + b'\x07')
    wait(lambda: cmd('display-message', '-p', '-t', pane,
                     '#{pane_title}') == tag, tag)
    if settle:
        time.sleep(.13)
    return snapshot()

def report(body, settle=True, term=b'\x07'):
    return send(b'\x1b]7501;' + body.encode() + term, settle)

def records():
    return snapshot()['records']

def reset():
    send(b'\x1bc')
    assert records() == []

def check(body, expected):
    assert report(body)['records'] == expected, (body, snapshot(), expected)

assert snapshot() == {'serial': 0, 'records': []}
check('state=working', [{'id': '', 'state': 'working'}])
check(' state = blocked : kind=permission:progress=042:id=build/test:app=cargo:'
      'title=UGxhbg==:msg=5a6J5YWo',
      [{'id': '', 'state': 'working'}, {'id': 'build/test', 'state': 'blocked',
        'app': 'cargo', 'kind': 'permission', 'progress': 42,
        'title': 'UGxhbg==', 'msg': '5a6J5YWo'}])
reset()
check('garbage:=x:Bad=1:future=yes:app=b@d:state=idle:state=done:app=a:app=b',
      [{'id': '', 'state': 'done', 'app': 'b'}])
check('state=working:kind=auth:app=a/b:progress=101:msg=:title=',
      [{'id': '', 'state': 'working', 'title': '', 'msg': ''}])
for progress in ['', '-1', '+1', '4.5', '1_0', '999999999999999999999']:
    check('state=blocked:kind=dance:progress=' + progress,
          [{'id': '', 'state': 'blocked'}])
for body in ['app=x', 'state=sleeping', 'state=', 'state=IDLE',
             'state=done:id=', 'state=done:id=/a', 'state=done:id=a/',
             'state=done:id=a//b', 'state=done:id=a,b', 'state=done:id=a=b',
             'state=done:id=' + 'a' * 33, 'state=done:id=a/b/c/d/e/f/g/h/i',
             'state=done:id=' + ('a' * 31 + '/') * 4 + 'a',
             'state=done:msg=a', 'state=done:msg=QQ=', 'state=done:msg=QQ===',
             'state=done:msg=YQpi', 'state=done:msg=wps=', 'state=done:msg=/w==',
             'state=done:msg=7aCA', 'state=done:msg=8J+Y', 'state=done:msg=9ICA',
             'state=done:msg=YQpi:msg=QQ==', 'state=done:title=a:title=QQ==',
             'state=done:app=' + 'x' * 33 + ':app=x',
             'state=done:' + 'x' * 17 + '=1',
             'state=done:' + 'x' * 17 + '=@',
             'state=done:msg=' + 'x' * 685 + ':msg=',
             'state=done:title=' + 'x' * 257 + ':title=',
             'state=done:app=' + 'x' * 33 + '@:app=x']:
    before = snapshot()
    assert report(body) == before, (body, before, snapshot())
check('state=done:msg=UGxhbg', [{'id': '', 'state': 'done', 'msg': 'UGxhbg'}])
msg512 = base64.b64encode(b'a' * 512).decode()
check('state=done:msg=' + msg512, [{'id': '', 'state': 'done', 'msg': msg512}])
before = snapshot()
assert report('state=error:msg=' + base64.b64encode(b'a' * 513).decode()) == before
title192 = base64.b64encode(b'a' * 192).decode()
check('state=done:title=' + title192,
      [{'id': '', 'state': 'done', 'title': title192}])
before = snapshot()
assert report('state=error:title=' + base64.b64encode(b'a' * 193).decode()) == before
for term in [b'\x07', b'\x1b\\']:
    body = 'state=done:future=' + 'x' * (4096 - 7 - len(term) - len('state=done:future='))
    assert report(body, term=term)['records'] == [{'id': '', 'state': 'done'}]
    before = snapshot()
    assert report(body + 'x', term=term) == before
before = snapshot()
send(b'\x1b]7501;state=error:future=' + b'x' * 100000 + b'\x07')
assert snapshot() == before
send(b'\x1b]4294974797;state=error\x07')
assert snapshot() == before
print('parser: PASS (valid, invalid, duplicate, limits, UTF-8, overflow, resync)')

reset()
report('state=done:app=pi')
report('state=done:id=a')
report('state=done:id=a/b')
report('state=done:id=ab')
report('state=clear:id=a')
assert [r['id'] for r in records()] == ['', 'ab']
report('state=clear')
assert records() == []
report('state=done:id=parent')
report('state=done:id=parent/child')
for i in range(62):
    report('state=done:id=r%02d' % i, settle=False)
time.sleep(.15)
assert len(records()) == 64
report('state=done:id=extra')
assert 'parent' not in [r['id'] for r in records()]
assert 'parent/child' in [r['id'] for r in records()]
report('state=done:id=parent/child')
report('state=done:id=extra2')
assert 'r00' not in [r['id'] for r in records()]
assert 'parent/child' in [r['id'] for r in records()]
assert [r['id'] for r in records()] == sorted(r['id'] for r in records())
print('store: PASS (subtree clear, boundaries, root clear, 64, update-LRU, sorting)')

reset()
for state in ['idle', 'working', 'blocked', 'done', 'error']:
    report('state=%s:id=%s' % (state, state))
original = records()
send(b'\x1b[?1049h\x1b[?1049l\x1b[!p\x1b]133;D;0\x07\x1b]133;N\x07')
assert records() == original
send(b'\x1b]133;A\x07')
assert [r['state'] for r in records()] == ['done', 'error']
reset()
send(b'\x1b]9;4;3\x07')
assert records() == [{'id': '', 'state': 'working'}]
send(b'\x1b]9;4;1;42\x07')
assert records() == [{'id': '', 'state': 'working', 'progress': 42}]
send(b'\x1b]9;4;2;55\x07')
assert records() == [{'id': '', 'state': 'error'}]
send(b'\x1b]9;4;0\x07')
assert records() == [{'id': '', 'state': 'idle'}]
report('state=blocked:kind=auth:msg=QQ==')
before = snapshot()
send(b'\x1b]9;4;0\x07')
assert snapshot() == before
report('state=clear')
send(b'\x1b]9;4;3\x07')
assert records() == []
reset()
send(b'\x1b]9;4;3\x07')
assert records() == [{'id': '', 'state': 'working'}]
other = cmd('new-window', '-d', '-P', '-F', '#{pane_id}',
            "printf '\033]9;4;3\007'; exec sleep 30")
wait(lambda: json.loads(cmd('display-message', '-p', '-t', other,
                            '#{pane_program_status}'))['records'], 'other pane fallback')
assert json.loads(cmd('display-message', '-p', '-t', other,
                     '#{pane_program_status}'))['records'] == [{'id': '', 'state': 'working'}]
cmd('kill-pane', '-t', other)
print('lifetime/fallback: PASS (A not D/N, alt/DECSTR, RIS, 9;4, pane-local)')

# These processes finish immediately after their last PTY write.
for state in ['working', 'blocked', 'idle', 'done', 'error']:
    cmd('respawn-pane', '-k', '-t', pane, "printf '\033]7501;state=%s\007'" % state)
    wait(lambda: cmd('display-message', '-p', '-t', pane, '#{pane_dead}') == '1', 'exit')
    time.sleep(.15)
    assert records() == ([] if state in ['working', 'blocked', 'idle'] else
                         [{'id': '', 'state': state}]), (state, snapshot())
cmd('respawn-pane', '-k', '-t', pane, 'exec sleep 30')
time.sleep(.15)
assert records() == []
print('exit/respawn: PASS (drained final done/error, transient cleanup, respawn)')

for term in [b'\x07', b'\x1b\\']:
    query = b'\x1b]7501;?' + term + b'\x1b[c'
    expected = b'\x1b]7501;?' + term + b'\x1b[?1;2c'
    output = root + '/reply'
    code = "import os,tty,select,time; tty.setraw(0); os.write(1,%r); " \
           "b=b''; end=time.monotonic()+2; " \
           "exec('while time.monotonic()<end and len(b)<%d:\\n" \
           " if select.select([0],[],[],.1)[0]: b+=os.read(0,1024)'); " \
           "open(%r,'wb').write(b)" % (query, len(expected), output)
    cmd('respawn-pane', '-k', '-t', pane,
        ' '.join(map(shlex.quote, [sys.executable, '-c', code])))
    wait(lambda: os.path.exists(output), 'query')
    assert open(output, 'rb').read() == expected
    os.unlink(output)
# Leave an outer-terminal request unanswered: the local query must wait behind
# it and still beat the later DA reply when that request times out.
pid, fd = os.forkpty()
if pid == 0:
    os.environ.pop('TMUX', None)
    os.environ.pop('TMUX_PANE', None)
    os.environ['TERM'] = 'xterm-256color'
    os.execl(tmux, tmux, '-S', socket, 'attach-session', '-t', 'status')
os.set_blocking(fd, False)
wait(lambda: cmd('list-clients', '-F', '#{client_tty}'), 'attached tty')
query = b'\x1b]4;1;?\x1b\\\x1b]7501;?\x07\x1b[c'
expected = b'\x1b]7501;?\x07\x1b[?1;2c'
code = "import os,tty,select,time; tty.setraw(0); os.write(1,%r); " \
       "b=b''; end=time.monotonic()+2; " \
       "exec('while time.monotonic()<end and len(b)<%d:\\n" \
       " if select.select([0],[],[],.1)[0]: b+=os.read(0,1024)'); " \
       "open(%r,'wb').write(b)" % (query, len(expected), output)
started = time.monotonic()
cmd('respawn-pane', '-k', '-t', pane,
    ' '.join(map(shlex.quote, [sys.executable, '-c', code])))
wait(lambda: os.path.exists(output), 'queued query')
assert time.monotonic() - started >= .4
assert open(output, 'rb').read() == expected
outer = b''
end = time.monotonic() + 1
while time.monotonic() < end and b'\x1b]4;1;?\x1b\\' not in outer:
    if select.select([fd], [], [], .05)[0]:
        outer += os.read(fd, 65536)
assert b'\x1b]4;1;?\x1b\\' in outer, outer
os.unlink(output)
os.kill(pid, signal.SIGTERM)
wait(lambda: os.waitpid(pid, os.WNOHANG)[0], 'tty client exit')
os.close(fd)
print('query: PASS (BEL/ST, detached pane, queued behind pending request, ordered before DA)')

os.close(writer)
cmd('respawn-pane', '-k', '-t', pane, command)
writer = os.open(fifo, os.O_WRONLY)
# Attach to a DIFFERENT session: status notifications must be server-wide.
cmd('new-session', '-d', '-s', 'observer', 'exec sleep 120')

def control():
    proc = subprocess.Popen([tmux, '-S', socket, '-C', 'attach-session', '-t', 'observer'],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    os.set_blocking(proc.stdout.fileno(), False)
    clients.append(proc)
    return proc

def drain(proc, duration=.05):
    data = b''
    end = time.monotonic() + duration
    while time.monotonic() < end:
        if select.select([proc.stdout], [], [], max(0, end-time.monotonic()))[0]:
            chunk = os.read(proc.stdout.fileno(), 1000000)
            if not chunk:
                break
            data += chunk
    return data

def statuses(data):
    result = []
    for line in data.splitlines():
        if line.startswith(b'%program-status '):
            _, p, serial, payload = line.split(b' ', 3)
            value = json.loads(payload)
            assert int(serial) == value['serial']
            assert p.decode() == pane
            result.append(value)
    return result

c1, c2 = control(), control()
drain(c1); drain(c2)
report('state=done:app=pi', settle=False)
leading = statuses(drain(c1, .035))
assert len(leading) == 1 and leading[0]['records'] == [{'id': '', 'state': 'done', 'app': 'pi'}]
for i in range(40):
    os.write(writer, b'\x1b]7501;state=working:progress=' + str(i).encode() + b'\x07')
last = report('state=blocked:kind=question:msg=QQ==', settle=False)
updates = statuses(drain(c1, .25))
assert 1 <= len(updates) <= 3, updates
assert updates[-1] == snapshot()
assert updates[-1]['records'] == [{'id': '', 'state': 'blocked', 'kind': 'question', 'msg': 'QQ=='}]
assert statuses(drain(c2))[-1] == snapshot()
assert updates[-1]['serial'] > leading[0]['serial']

# Spaced writes cannot collapse merely because one PTY read contained them all.
time.sleep(.12)
cadence = b''
# Written directly: a round trip per write takes longer than the throttle
# window under a sanitizer build.
for i in range(12):
    os.write(writer, b'\x1b]7501;state=working:progress=%d\x07' % i)
    cadence += drain(c1, .025)
report('state=working:progress=11', settle=False)
cadence += drain(c1, .15)
assert 2 <= len(statuses(cadence)) < 8, len(statuses(cadence))
assert statuses(cadence)[-1] == snapshot()

# Open a guard for a command which keeps producing output while statuses change.
c1.stdin.write(b"run-shell 'for i in 1 2 3 4 5; do echo guard; sleep 0.1; done'\n")
c1.stdin.flush()
time.sleep(.03)
report('state=done:msg=Qg==', settle=False)
guarded = drain(c1, .7)
depth = 0
for line in guarded.splitlines():
    if line.startswith(b'%begin '): depth += 1
    if line.startswith((b'%end ', b'%error ')): depth -= 1
    if line.startswith(b'%program-status '): assert depth == 0, guarded
assert statuses(guarded)[-1] == snapshot()

# Stop reading: the socket fills, but updates must collapse to one unsent slot.
slow = control()
drain(slow)
large = 'state=working:title=' + title192 + ':msg=' + msg512
for batch in range(16):
    for i in range(64):
        os.write(writer, ('\x1b]7501;' + large + ':id=r%02d\x07' % i).encode())
    time.sleep(.11)
report('state=clear', settle=False)
report('state=done:app=final', settle=False)
time.sleep(.15)
expected = snapshot()
data = drain(slow, 1)
assert statuses(data)[-1] == expected
assert 1 <= len(statuses(data)) <= 4, len(statuses(data))
# Fill the socket again, then destroy a pane with an unsent final payload.
for batch in range(16):
    for i in range(64):
        os.write(writer, ('\x1b]7501;' + large + ':id=r%02d\x07' % i).encode())
    time.sleep(.11)
report('state=clear', settle=False)
report('state=done:app=discarded', settle=False)
time.sleep(.15)
discarded = snapshot()['serial']
cmd('kill-pane', '-t', pane)
assert discarded not in [r['serial'] for r in statuses(drain(slow, 1))]
for proc in clients:
    proc.stdin.write(b'detach-client\n'); proc.stdin.flush()
    os.set_blocking(proc.stdout.fileno(), True)
    proc.communicate(timeout=4)
print('control/format: PASS (server-wide, leading/trailing throttle, serial, guards, slow-reader coalescing/flush, pane destruction)')
os.close(writer)
PY
