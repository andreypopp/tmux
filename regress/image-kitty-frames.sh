#!/bin/sh
PATH=/bin:/usr/bin
TERM=screen
export TERM
[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
command -v python3 >/dev/null || exit 101
python3 -B - "$TEST_TMUX" <<'PY'
import _posixshmem as shm
import base64
import mmap
import os
import re
import sys
from image_test import image_test, kitty, png, Control, SharedMemory, shm_contents, token

with image_test(sys.argv[1]) as test:
    objects = SharedMemory()
    control = None
    try:
        test.record()
        pane = test.pane
        capture = test.capture
        reply = test.reply
        recorded = test.recorded
        def pixels(identity=1):
            selected = [value for keys, value in test.frames() if b'i=%d,' % identity in keys]
            assert len(selected) == 1, selected
            return selected[0]
        red = b'\xff\0\0\xff'
        green = b'\0\xff\0\xff'
        original = red * 16
        client = test.attach(cw=1, ch=1)
        test.emit(kitty('a=T,i=1,f=100,c=4,r=4,C=1', png(4, 4, [red * 4] * 4)))
        assert client.images(), 'initial sixel not drawn'
        reply('a=f,i=1,r=1,x=1,y=1,s=2,v=2,X=1,f=32', green * 4, b'\033_Gi=1,r=1;OK\033\\')
        expected = bytearray(original)
        for y in (1, 2):
            expected[(y * 4 + 1) * 4:(y * 4 + 3) * 4] = green * 2
        assert pixels() == expected
        test.wait(lambda: any((0, 255, 0) in image[2] for image in client.images()))
        test.detach(client)
        print('PASS root frame pixels and scheduled sixel redraw')

        for under, over, expected in [
            (bytes([20, 40, 60, 128]), bytes([200, 100, 50, 128]), bytes([140, 80, 53, 192])),
            (bytes([255, 255, 255, 2]), bytes([255, 255, 255, 2]), bytes([255, 255, 255, 3])),
            (bytes([20, 30, 40, 0]), bytes([80, 90, 100, 0]), bytes([80, 90, 100, 0])),
        ]:
            test.emit(kitty('a=T,i=1,f=32,s=4,v=4,c=4,r=4,C=1', under * 16))
            test.emit(kitty('a=f,i=1,r=1,c=999,Y=4278190335,f=32,s=1,v=1,m=1', over[:3]))
            test.emit(kitty('m=0', over[3:]))
            assert pixels() == expected + under * 15
        test.emit(kitty('a=f,i=1,r=1,c=1,Y=1,X=1,f=24,s=1,v=1,x=3,y=3', b'\x01\x02\x03'))
        assert pixels()[-4:] == b'\x01\x02\x03\xff'
        over = bytes([200, 100, 50, 128])
        test.emit(kitty('a=f,i=1,r=1,X=1,f=32,s=1,v=1', over))
        assert pixels()[:4] == over
        test.emit(kitty('a=f,i=1,r=1,X=1,f=100,x=3,y=3', png(2, 2, [green * 2] * 2)))
        before = pixels()
        assert before[-4:] == green
        test.emit(kitty('a=f,i=1,r=1,X=1,f=32,s=1,v=1,x=4294967295', green))
        assert pixels() == before
        print('PASS exact Ghostty blend, overwrite, c/Y edit semantics and clipping')

        recorded(kitty('a=f,i=1,r=1,f=32,s=1,v=1,X=1,m=1,q=2', green[:1]) +
            kitty('a=f,i=999,r=99,x=999,y=999,s=99,v=99,m=0,q=0', green[1:]) + b'\033[c', b'\033[?1;2c')
        assert pixels()[:4] == green
        recorded(kitty('a=f,i=1,r=1,f=32,s=1,v=1,X=1,m=1,q=0', over[:1]) +
            kitty('m=1,q=1', over[1:2]) + kitty('m=0,q=0', over[2:]) + b'\033[c', b'\033[?1;2c')
        assert pixels()[:4] == over
        recorded(kitty('a=T,i=1,f=32,s=4,v=4,c=4,r=4,C=1,m=1,q=2', original[:3]) +
            kitty('i=999,s=99,v=99,m=0,q=0', original[3:]) + b'\033[c', b'\033[?1;2c')
        assert pixels() == original
        print('PASS chunked commands retain first keys and sticky nonzero quiet')

        for keys, error in [
            ('i=99,r=1,s=1,v=1', 'ENOENT'),
            ('i=1,r=1,s=5,v=1', 'EINVAL'),
            ('i=1,r=1,s=1', 'EINVAL'),
        ]:
            identity = 99 if 'i=99,' in keys else 1
            number = int(re.search(r'r=(\d+)', keys)[1])
            frame = ',r=%d' % number if number else ''
            reply('a=f,f=32,' + keys, green * (5 if 's=5' in keys else 1),
                ('\033_Gi=%d%s;%s\033\\' % (identity, frame, error)).encode())
        test.run('set-option', '-s', 'image-size-limit', '32')
        reply('a=f,i=1,r=1,f=32,s=4,v=4', green * 16, b'\033_Gi=1,r=1;EINVAL\033\\')
        test.run('set-option', '-s', 'image-size-limit', '67108864')
        print('PASS frame errors and payload limit')

        control = Control(test, 'ignore-size')
        name = objects.create(original)
        shared = shm.shm_open(name, os.O_RDWR, 0)
        try:
            with mmap.mmap(shared, len(original)) as view:
                test.emit(kitty('a=T,i=2,f=32,s=4,v=4,c=4,r=4,C=1,t=s', name.encode()) +
                    kitty('a=f,i=2,r=1,f=32,s=1,v=1,X=1', green))
                wire = control.sync()
                transmitted = re.findall(rb'\033_Ga=T,t=d,i=2,[^;]*;(.*?)\033\\', wire)
                assert len(transmitted) == 1 and base64.b64decode(transmitted[0]) == original, wire
                assert pixels(2) == green + original[4:]
                assert view[:] == original, 'program shared memory was written'
        finally:
            os.close(shared)
        print('PASS shared-memory private edit and queued original payload')
        name = objects.create(green)
        test.emit(kitty('a=f,i=2,r=1,c=1,Y=123,x=1,y=1,z=25,X=1,f=32,s=1,v=1,t=s', name.encode()))
        wire = control.sync()
        keys, value = re.search(rb'\033_G(a=f,t=d,i=2,[^;]*);(.*?)\033\\', wire).groups()
        for key in [b'r=1', b'c=1', b'Y=123', b'x=1', b'y=1', b'z=25', b'X=1', b'f=32,s=1,v=1,']:
            assert key in keys, (key, keys)
        assert base64.b64decode(value) == green
        edited = pixels(2)
        assert edited[20:24] == green
        control.exchange('refresh-client -f image-shm')
        restored = control.exchange('capture-pane -K -p -t ' + pane)
        keys, value = re.search(rb'\033_G(a=t,i=2,[^;]*);(.*?)\033\\', restored).groups()
        assert b'f=32,s=4,v=4,t=s,S=64,q=2' in keys, keys
        name = base64.b64decode(value).decode()
        objects.names.append(name)
        assert shm_contents(name) == edited
        control.close()
        control = None
        print('PASS shared-memory frame re-emit and edited restore')

        test.emit(kitty('a=d,d=A'))
        test.run('set-option', '-s', 'image-total-size-limit', '64')
        for medium in ['d', 's']:
            name = objects.create(original)
            test.emit(kitty('a=T,i=1,f=32,s=4,v=4,c=4,r=4,C=1,t=' + medium,
                original if medium == 'd' else name.encode()))
            edit = objects.create(green)
            reply('a=f,i=1,r=1,f=32,s=1,v=1,X=1,t=s', edit.encode(), b'\033_Gi=1,r=1;OK\033\\')
            assert pixels() == green + original[4:]
            test.emit(kitty('a=d,d=A'))
        test.run('set-option', '-s', 'image-total-size-limit', '128')
        test.emit(b'\033[H' + kitty('a=T,i=1,f=32,s=4,v=4,c=4,r=4,C=1', original) +
            b'\033[1;6H' + kitty('a=T,i=2,f=32,s=4,v=4,c=4,r=4,C=1', original))
        captured = capture()
        assert b'i=1,' in captured and b'i=2,' in captured, 'raw RGBA charged twice'
        test.run('set-option', '-s', 'image-total-size-limit', '268435456')
        print('PASS full-quota shared-memory edits and raw single-buffer accounting')
        test.emit(kitty('a=d,d=A'))
        name = objects.create(red * (2048 * 1024))
        test.emit(kitty('a=T,i=3,f=32,s=2048,v=1024,c=1,r=1,C=1,t=s', name.encode()))
        continuation = token(capture())
        test.emit(kitty('a=f,i=3,r=1,f=32,s=1,v=1,X=1', green))
        result = capture('-k', continuation, check=False)
        assert result.returncode != 0 and b'image changed' in result.stderr, result
        print('PASS continuation rejects an edited root')
    finally:
        if control is not None:
            control.close()
        objects.close()
PY
