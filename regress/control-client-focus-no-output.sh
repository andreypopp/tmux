#!/bin/sh

PATH=/bin:/usr/bin
TERM=screen
export TERM
[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
command -v python3 >/dev/null || exit 101

python3 -B - "$TEST_TMUX" <<'PY'
import os
import subprocess
import sys
import tempfile
from image_test import ImageTest

with tempfile.TemporaryDirectory(prefix='/tmp/control-focus-') as directory:
    test = ImageTest(sys.argv[1])
    control = None
    try:
        test.run('new-session', '-d', '-s', 'image', '-x', '40', '-y', '16', 'sleep 60')
        test.run('set-option', '-g', 'status', 'off')
        test.run('set-option', '-g', 'focus-events', 'on')
        pane = test.run('display-message', '-p', '#{pane_id}')
        path = directory + '/input'
        test.run('respawn-pane', '-k', '-t', pane,
            "stty raw -echo; printf '\033[?1004h\033]2;recorder\007'; exec cat > " + path)
        test.wait(lambda: test.run('display-message', '-p', '-t', pane,
            '#{pane_title}') == 'recorder')
        client = test.attach(graphical=False)
        control = subprocess.Popen(test.server + ['-C', 'attach-session', '-E',
            '-t', 'image', '-f', 'no-output,ignore-size'],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL)
        test.wait(lambda: str(control.pid) in test.run('list-clients', '-F', '#{client_pid}').splitlines())
        other = test.run('split-window', '-d', '-P', '-F', '#{pane_id}', 'sleep 60')
        expected = b'\033[I'
        def received():
            with open(path, 'rb') as f:
                return f.read()
        test.wait(lambda: received() == expected)
        test.run('select-pane', '-t', other)
        expected += b'\033[O'
        test.wait(lambda: received() == expected)
        test.run('select-pane', '-t', pane)
        expected += b'\033[I'
        test.wait(lambda: received() == expected)
        test.run('new-session', '-d', '-s', 'other', 'sleep 60')
        test.run('switch-client', '-c', client.name, '-t', 'other')
        expected += b'\033[O'
        try:
            test.wait(lambda: received() == expected)
        except RuntimeError as e:
            raise AssertionError(('switch-client did not remove focus', received(), expected)) from e
        test.run('switch-client', '-c', client.name, '-t', 'image')
        expected += b'\033[I'
        test.wait(lambda: received() == expected)
        name = 'client-%d' % control.pid
        test.run('refresh-client', '-t', name, '-f', '!no-output')
        test.run('switch-client', '-c', client.name, '-t', 'other')
        test.run('switch-client', '-c', name, '-t', 'other')
        expected += b'\033[O'
        test.wait(lambda: received() == expected)
        test.run('switch-client', '-c', name, '-t', 'image')
        expected += b'\033[I'
        test.wait(lambda: received() == expected)
    finally:
        if control is not None:
            control.stdin.close()
            control.wait(timeout=10)
        test.close()
PY
