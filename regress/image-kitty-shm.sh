#!/bin/sh
PATH=/bin:/usr/bin
TERM=screen
export TERM
[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
command -v python3 >/dev/null || exit 101
python3 -B - "$TEST_TMUX" <<'PY'
import _posixshmem as shm
import base64
import os
import re
import signal
import subprocess
import sys
import time
import zlib
from image_test import image_test, kitty, png, Control, SharedMemory
from image_test import shm_exists as exists, shm_contents as contents

objects = SharedMemory()
create, names = objects.create, objects.names

with image_test(sys.argv[1]) as test:
    controls = []
    try:
        pane = test.run('display-message', '-p', '#{pane_id}')
        # A no-output client keeps pane reads enabled while output clients are blocked.
        monitor = Control(test, 'no-output,ignore-size')
        controls.append(monitor)
        direct = Control(test, 'ignore-size')
        controls.append(direct)
        data = b'\xff\0\0\xff' * 16
        name = create(data)
        raw = kitty('a=T,i=41,p=3,f=32,s=4,v=4,c=2,r=2,x=1,y=1,w=2,h=2,X=1,Y=1,C=1,z=-3,t=s,q=2', name.encode())
        test.emit(raw)
        assert not exists(name), 'unflagged upload not unlinked'
        output = direct.sync()
        apcs = re.findall(rb'\033_G(.*?);(.*?)\033\\', output)
        assert len(apcs) == 2, output
        assert apcs[0][0].endswith(b't=s,q=2'), apcs
        assert b'a=T,t=d,i=41,I=0,p=3,f=32,s=4,v=4,' in apcs[1][0], apcs
        for key in [b'c=2', b'r=2', b'x=1', b'y=1', b'w=2', b'h=2', b'X=1', b'Y=1', b'C=1', b'z=-3', b'q=2']:
            assert key in apcs[1][0], (key, apcs)
        assert base64.b64decode(apcs[1][1]) == data
        test.emit(b'LATER')
        assert direct.sync().endswith(b'LATER\033]2;image-2\007')
        large = b'\x12\x34\x56\xff' * (64 * 64)
        name = create(large)
        test.emit(kitty('a=t,i=40,f=32,s=64,v=64,t=s,q=2', name.encode()))
        chunks = re.findall(rb'\033_G([^;]*);(.*?)\033\\', direct.sync())[1:]
        assert len(chunks) == 6 and b'a=t,t=d,i=40,I=0,' in chunks[0][0], chunks
        assert all(b'q=2' in keys for keys, _ in chunks)
        assert b'm=0' in chunks[-1][0]
        assert b''.join(base64.b64decode(value) for _, value in chunks) == large
        assert not exists(name)
        for identity, tail in [(61, kitty('a=d,d=I,i=61')),
                (62, b'\033[5;9HMOVED'),
                (63, b'\033_Ga=t,i=99,q=2;AAAA')]:
            name = create(data)
            raw = kitty('a=T,i=%d,f=32,s=4,v=4,C=1,t=s,q=2' % identity, name.encode())
            fd = os.open(test.run('display-message', '-p', '#{pane_tty}'), os.O_WRONLY)
            try:
                assert os.write(fd, raw + tail) == len(raw + tail)
            finally:
                os.close(fd)
            received = bytearray()
            def complete():
                received.extend(direct.sync())
                return tail in received
            test.wait(complete)
            emitted = (b'\033_Ga=T,t=d,i=%d,I=0,p=0,f=32,s=4,v=4,' % identity +
                b'x=0,y=0,w=0,h=0,X=0,Y=0,c=0,r=0,C=1,q=2,S=0,z=0,m=0;' +
                base64.b64encode(data) + b'\033\\')
            assert bytes(received) == raw + emitted + tail, received
            if identity == 63:
                test.emit(b'\033\\')
                direct.sync()
            else:
                test.emit(b'\033[H')
                direct.sync()
            assert b'i=61,' not in subprocess.check_output(test.server +
                ['capture-pane', '-K', '-p', '-t', pane])
            test.emit(kitty('a=d,d=I,i=%d' % identity))
            direct.sync()
        print('PASS shm APC boundary order')
        local = Control(test, 'image-shm,ignore-size')
        controls.append(local)
        name = create(data)
        test.emit(kitty('a=T,i=42,f=32,s=4,v=4,t=s,q=2,C=1', name.encode()))
        output = local.sync()
        assert b't=s' in output and b't=d' not in output, output
        assert exists(name), 'guard unlinked before client read'
        assert b't=d' in direct.sync()
        first_guard = name
        line = local.exchange('capture-pane -K -p -t ' + pane)
        records = re.findall(rb'\033_G([^;]*);(.*?)\033\\', line)
        assert len(records) == 2 and all(b't=s,' in keys and b'm=' not in keys for keys, _ in records), records
        for keys, encoded in records:
            restored = base64.b64decode(encoded).decode()
            names.append(restored)
            assert contents(restored) == data, (keys, restored, contents(restored))
        captured = subprocess.check_output(test.server + ['capture-pane', '-K', '-p', '-t', pane])
        assert all(base64.b64decode(value) == data for value in re.findall(rb';(.*?)\033\\', captured)), captured
        replacement = b'\0\xff\0\xff' * 16
        name = create(replacement)
        test.emit(kitty('a=T,i=42,f=32,s=4,v=4,t=s,q=2,C=1', name.encode()))
        local.sync()
        line = local.exchange('capture-pane -K -p -t ' + pane)
        encoded = re.search(rb'i=42,[^;]*;(.*?)\033\\', line)[1]
        restored = base64.b64decode(encoded).decode()
        names.append(restored)
        assert contents(restored) == replacement
        name = create(data, 'kis%d-bare' % os.getpid())
        test.emit(kitty('a=T,i=43,f=32,s=4,v=4,t=s,q=2,C=1', name.encode()))
        assert b'i=43,' in local.exchange('capture-pane -K -p -t ' + pane)
        name = create(data)
        apc = kitty('a=T,i=44,f=32,s=4,v=4,t=s,q=2,C=1', name.encode())
        test.send(apc[:-1])
        test.run('display-message', '-p', 'split')
        test.emit(apc[-1:])
        assert b'i=44,' in local.exchange('capture-pane -K -p -t ' + pane)
        direct.sync()
        local.sync()
        shm.shm_unlink(name)
        name = create(data, name)
        test.emit(b'x' * (256 * 1024))
        test.emit(kitty('a=T,i=45,f=32,s=4,v=4,t=s,q=2,C=1', name.encode()))
        end = time.monotonic() + 1.2
        def held():
            assert exists(name), 'guard ran while block was queued or old guard unlinked reused name'
            return time.monotonic() >= end
        test.wait(held)
        output = local.sync()
        assert b'i=45,' in output and exists(name), 'name unavailable when raw APC read'
        drained_guard = name
        assert not exists(first_guard)
        direct.sync()

        offset = create(b'junk' + data)
        test.emit(kitty('a=T,i=46,f=32,s=4,v=4,t=s,O=4,S=64,q=2,C=1', offset.encode()))
        capture = subprocess.check_output(test.server + ['capture-pane', '-K', '-p', '-t', pane])
        assert base64.b64decode(re.search(rb'i=46,[^;]*;(.*?)\033\\', capture)[1]) == data
        for identity, keys, value in [
            (48, 'f=24,s=4,v=4', b'\xff\0\0' * 16),
            (49, 'f=32,s=4,v=4,o=z', zlib.compress(data)),
            (50, 'f=100', png(4, 4, [b'\xff\0\0\xff' * 4] * 4)),
            (51, 'f=100,o=z', zlib.compress(png(4, 4, [b'\xff\0\0\xff' * 4] * 4))),
        ]:
            name = create(value)
            test.emit(kitty('a=T,i=%d,%s,t=s,S=%d,q=2,C=1' %
                (identity, keys, len(value)), name.encode()))
            capture = subprocess.check_output(test.server + ['capture-pane', '-K', '-p', '-t', pane])
            encoded = re.search(rb'i=' + str(identity).encode() + rb',[^;]*;(.*?)\033\\', capture)[1]
            assert base64.b64decode(encoded) == value, (identity, capture)
        local.sync()
        direct.sync()
        line = local.exchange('capture-pane -K -p -t ' + pane)
        exit_names = [base64.b64decode(value).decode() for value in
            re.findall(rb';(.*?)\033\\', line)]
        names.extend(exit_names)
        assert exit_names and all(exists(value) for value in exit_names)
        test.wait(lambda: not exists(drained_guard))
        print('PASS shm name reuse and queued guard')
        test.emit(b'x' * (256 * 1024))
        test.run('set-option', '-s', 'image-total-count-limit', '1')
        one, two = create(data), create(replacement)
        server_pid = int(test.run('display-message', '-p', '#{pid}'))
        try:
            test.emit(kitty('a=t,i=81,f=32,s=4,v=4,t=s,q=2', one.encode()) +
                kitty('a=t,i=82,f=32,s=4,v=4,t=s,q=2', two.encode()))
            assert test.run('display-message', '-p', 'alive') == 'alive'
        except (subprocess.TimeoutExpired, RuntimeError):
            os.kill(server_pid, signal.SIGKILL)
            raise
        test.run('set-option', '-s', 'image-total-count-limit', '1000')
        queued = direct.sync()
        assert b'i=81,' in queued and b'i=82,' in queued
        local.sync()
        print('PASS shm queued-image eviction')
        test.emit(b'x' * (256 * 1024))
        name = create(data)
        test.emit(kitty('a=t,i=47,f=32,s=4,v=4,t=s,q=2', name.encode()))
        assert exists(name)
        test.run('kill-session', '-t', 'image')
        for control in controls:
            control.close()
        controls.clear()
        test.wait(lambda: not exists(name) and all(not exists(value) for value in exit_names))
        print('PASS image-kitty-shm')
    finally:
        for control in controls:
            control.close()
        objects.close()
PY
