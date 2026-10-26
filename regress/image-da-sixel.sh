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
from image_test import image_test

with image_test(sys.argv[1]) as test, tempfile.TemporaryDirectory() as directory:
    control = subprocess.Popen(test.server + ['-C', 'attach-session', '-t', 'image',
        '-f', 'no-output,ignore-size'], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL)
    try:
        def query(expected):
            path = os.path.join(directory, 'reply')
            if os.path.exists(path):
                os.unlink(path)
            test.run('new-window', '-d', 'stty raw -echo; printf "\033[c"; '
                'dd bs=1 count=%d of=%s 2>/dev/null' % (len(expected), path))
            def received():
                if not os.path.exists(path):
                    return False
                with open(path, 'rb') as file:
                    return file.read() == expected
            test.wait(received)
        query(b'\033[?1;2c')
        test.attach(graphical=False)
        query(b'\033[?1;2c')
        test.attach(graphical=True)
        query(b'\033[?1;2;4c')
    finally:
        control.stdin.close()
        control.wait(timeout=10)
print('DA sixel: control only PASS; tty without sixel PASS; tty with sixel PASS')
PY
