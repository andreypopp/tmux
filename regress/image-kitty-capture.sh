#!/bin/sh

PATH=/bin:/usr/bin
TERM=screen
export TERM
[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
command -v python3 >/dev/null || exit 101

python3 -B - "$TEST_TMUX" <<'PY'
import base64
import os
import re
import select
import subprocess
import sys
import time
from image_test import image_test, kitty, png

with image_test(sys.argv[1]) as test:
    control = None
    try:
        pane = test.run('display-message', '-p', '#{pane_id}')
        data = png(32, 32, [b'\xff\0\0\xff' * 32] * 32,
            chunks=[(b'tEXt', b'capture\0' + b'x' * 4000)])
        small = png(1, 1, [b'\xff\0\0\xff'])
        test.emit(kitty('a=T,f=100,c=1,r=1,C=1,z=-1', small))
        def capture(flag, *extra):
            return subprocess.check_output(test.server + ['capture-pane', '-p',
                flag, '-t', pane] + list(extra))
        first = re.search(rb'a=p,i=(\d+),z=-1,', capture('-G')).group(1)
        assert first == b'2147483649', first
        test.emit(b'\033[2;1H' + kitty('a=T,f=100,c=1,r=1,C=1,z=-1073741825', small) +
            b'\033[3;1H' + kitty('a=T,i=' + first.decode() + ',f=100,c=1,r=1,C=1', small) +
            b'\033[4;1H' + kitty('a=p,i=' + first.decode() + ',p=4,c=1,r=1,C=1,z=5'))
        placement_data = capture('-G')
        placements = re.findall(rb'a=p,i=(\d+),(?:z=(-?\d+),)?', placement_data)
        assert placements == [(b'2147483650', b'-1'), (b'2147483651', b'-1073741825'),
            (first, b''), (first, b'5')], placements
        keys = re.findall(rb'\033_Ga=p,i=\d+,(.*?)\033\\', placement_data)
        assert all(b'p=' not in value for value in keys[:2]), keys
        assert re.findall(rb'a=t,i=(\d+),', capture('-K')) == [value[0] for value in placements[:3]]
        test.emit(kitty('a=d,d=A') + b'\033c')
        icat = png(8, 16, [b'\xff\0\0\xff' * 8] * 16)
        test.emit(kitty('a=T,f=100,s=8,v=16,X=4,C=1', icat))
        offset_id = re.search(rb'a=p,i=(\d+),', capture('-G')).group(1)
        assert capture('-G', '-S', '0', '-E', '0') == (
            b'\033_Ga=p,i=' + offset_id +
            b',X=4,q=2,C=1,c=0,r=0,x=0,y=0,w=8,h=16\033\\\n')
        test.emit(kitty('a=d,d=A') + b'\033c')
        tall = png(16, 48, [b'\xff\0\0\xff' * 16] * 48)
        test.emit(kitty('a=T,f=100,X=4,Y=3,C=1', tall))
        offset_id = re.search(rb'a=p,i=(\d+),', capture('-G')).group(1)
        assert capture('-G', '-S', '0', '-E', '1') == (
            b'\033_Ga=p,i=' + offset_id +
            b',X=4,Y=3,q=2,C=1,c=0,r=0,x=0,y=0,w=16,h=48\033\\\n\n')
        assert capture('-G', '-S', '1', '-E', '1') == (
            b'\033_Ga=p,i=' + offset_id +
            b',X=4,q=2,C=1,c=0,r=0,x=0,y=29,w=16,h=19\033\\\n')
        test.emit(kitty('a=d,d=A') + b'\033c')
        scaled = png(3, 2, [b'\xff\0\0\xff' * 3] * 2)
        test.emit(kitty('a=T,i=6,f=100,c=4,C=1', scaled))
        assert capture('-G', '-S', '1', '-E', '1') == (
            b'\033_Ga=p,i=6,q=2,C=1,c=4,r=1,x=0,y=1,w=3,h=1\033\\\n')
        test.emit(kitty('a=d,d=A') + b'\033c')
        test.emit(b'\033[Habc' + kitty('a=T,i=7,p=3,f=100,c=4,r=2,C=1', data) + b'text')
        placement = b'\033_Ga=p,i=7,p=3,q=2,C=1,c=4,r=2,x=0,y=0,w=32,h=32\033\\'
        expected = b'abc' + placement + b'text\n\n'
        actual = subprocess.check_output(test.server + ['capture-pane', '-G', '-p',
            '-t', pane, '-S', '0', '-E', '1'])
        assert actual == expected, (actual, expected)
        encoded = base64.b64encode(data)
        chunks = [encoded[i:i + 4096] for i in range(0, len(encoded), 4096)]
        expected = b''.join(b'\033_G' +
            (b'a=t,i=7,f=100,q=2,' if i == 0 else b'') +
            ((b'm=0' if i == len(chunks) - 1 else b'm=1') +
                (b';' if i == 0 else b',q=2;')) +
            chunk + b'\033\\\n' for i, chunk in enumerate(chunks))
        actual = subprocess.check_output(test.server + ['capture-pane', '-K', '-p', '-t', pane])
        assert actual == expected, (actual, expected)
        control = subprocess.Popen(test.server + ['-C', 'attach-session', '-t', 'image',
            '-f', 'no-output,ignore-size'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        os.set_blocking(control.stdout.fileno(), False)
        control.stdin.write(('capture-pane -K -p -t ' + pane + '\n').encode())
        control.stdin.flush()
        output = b''
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if select.select([control.stdout], [], [], 0.1)[0]:
                output += os.read(control.stdout.fileno(), 65536)
            if expected + b'%end ' in output:
                break
        assert expected + b'%end ' in output, output
        test.emit(kitty('a=d,d=I,i=7'))
        prefix = data[:3073]
        test.emit(b'\033[2J\033[Hpre' + kitty(
            'a=T,i=9,f=100,c=4,r=2,C=1,m=1', prefix))
        pending = b'pre\n\033_Ga=T,i=9,I=0,p=0,f=100,x=0,y=0,w=0,h=0,' + \
            b'X=0,Y=0,c=4,r=2,C=1,q=2,S=0,z=0,m=1;' + \
            base64.b64encode(prefix[:3072]) + b'\033\\\n\033_Gm=1,q=2;' + \
            base64.b64encode(prefix[3072:]) + b'\033\\\n'
        actual = subprocess.check_output(test.server + ['capture-pane', '-G', '-p',
            '-t', pane, '-S', '0', '-E', '0'])
        assert actual == pending, (actual, pending)
        test.emit(kitty('m=0', data[3073:]))
        actual = subprocess.check_output(test.server + ['capture-pane', '-K', '-p', '-t', pane])
        assert actual == expected.replace(b'i=7,', b'i=9,'), actual
        test.emit(kitty('a=d,d=A'))
        test.emit(b'\033[2J\033[Habcdefghij\033[1;11H' +
            kitty('a=T,i=11,f=100,c=1,r=1,C=1', data) + b'\033[1;3H' +
            kitty('a=T,i=12,f=100,c=1,r=1,C=1', data))
        first = b'\033_Ga=p,i=12,q=2,C=1,c=1,r=1,x=0,y=0,w=32,h=32\033\\'
        second = first.replace(b'i=12,', b'i=11,')
        actual = subprocess.check_output(test.server + ['capture-pane', '-G', '-p',
            '-t', pane, '-S', '0', '-E', '0'])
        assert actual == b'ab' + first + b'cdefghij' + second + b'\n', actual
        test.emit(kitty('a=d,d=A') + b'\033[2J\033[H\033[1;11H' +
            kitty('a=T,i=13,f=100,c=1,r=1,C=1', data))
        placement = first.replace(b'i=12,', b'i=13,')
        for flags in [[], ['-J'], ['-N'], ['-e']]:
            actual = subprocess.check_output(test.server + ['capture-pane', '-G', '-p',
                '-t', pane, '-S', '0', '-E', '0'] + flags)
            assert actual == b' ' * 10 + placement + b'\n', (flags, actual)
        test.emit(kitty('a=d,d=A'))
        test.run('set-option', '-g', 'image-total-size-limit', '4096')
        test.emit(kitty('a=T,i=10,f=100,c=4,r=2,C=1', data))
        actual = subprocess.check_output(test.server + ['capture-pane', '-K', '-p', '-t', pane])
        assert actual == b'', actual
    finally:
        if control is not None:
            control.stdin.close()
            control.wait(timeout=10)
PY
