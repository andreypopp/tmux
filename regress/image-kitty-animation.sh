#!/bin/sh
PATH=/bin:/usr/bin
TERM=screen
export TERM
[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
command -v python3 >/dev/null || exit 101
python3 -B - "$TEST_TMUX" <<'PY'
import os
import re
import sys
import time
from image_test import image_test, kitty, Control, SharedMemory, token

with image_test(sys.argv[1]) as test:
    objects = SharedMemory()
    control = None
    try:
        test.record()
        pane = test.pane
        capture = test.capture
        reply = test.reply
        frames = test.frames
        def state():
            keys = re.search(rb'\033_Ga=a,([^\033]*)\033\\', capture())[1]
            return {k.decode(): int(v) for k, v in (pair.split(b'=') for pair in keys.split(b','))}
        red = b'\xff\0\0\xff'
        green = b'\0\xff\0\xff'
        blue = b'\0\0\xff\xff'
        def root():
            test.emit(kitty('a=d,d=A') + kitty('a=T,i=1,f=32,s=2,v=2,c=2,r=2,C=1', red * 4))
        root()
        reply('a=f,i=1,r=99,c=1,f=32,s=1,v=1,x=1,y=1,z=75,X=1', green, b'\033_Gi=1,r=2;OK\033\\')
        reply('a=f,i=1,r=0,Y=65535,f=32,s=1,v=1,z=-1,X=1', green, b'\033_Gi=1,r=3;OK\033\\')
        test.emit(kitty('a=f,i=1,f=32,s=1,v=1,m=1', blue[:2]) + kitty('m=0', blue[2:]))
        restored = frames()
        assert [f[1] for f in restored] == [red * 4, red * 3 + green, green + blue * 3, blue + b'\0' * 12], restored
        assert [re.search(rb'z=(-?\d+)', f[0])[1] for f in restored[1:]] == [b'75', b'-1', b'40']
        assert b'a=t,' in restored[0][0] and all(b'a=f,' in f[0] for f in restored[1:])
        assert state() == {'i': 1, 'c': 1, 's': 1, 'v': 1, 'q': 2}
        reply('a=f,i=1,c=999,f=32,s=1,v=1', green, b'\033_Gi=1;EINVAL\033\\')
        test.emit(kitty('a=f,i=1,r=2,f=32,s=1,v=1', bytes([0, 0, 255, 128])))
        assert frames()[1][1][:4] == bytes([127, 0, 128, 255])
        test.emit(kitty('a=f,i=1,r=2,f=32,s=1,v=1,X=1,z=-1', blue))
        edited = frames()[1]
        assert edited[1][:4] == blue and b'z=-1,' in edited[0]
        print('PASS append numbering, base, background, blend, overwrite, gaps and chunking')

        reply('a=c,i=1,r=1,c=2,w=1,h=1,x=1,y=0,C=1', b'', b'\033_Gi=1;OK\033\\')
        assert frames()[1][1][4:8] == red
        reply('a=c,i=1,r=1,c=1,w=1,h=1', b'', b'\033_Gi=1;EINVAL\033\\')
        reply('a=c,i=1,r=99,c=1', b'', b'\033_Gi=1;ENOENT\033\\')
        reply('a=c,i=1,r=1,c=2,w=2,h=2,x=1', b'', b'\033_Gi=1;EINVAL\033\\')
        test.emit(kitty('a=a,i=1,c=3,s=1') + kitty('a=d,d=f,i=1,r=2'))
        assert len(frames()) == 3 and state()['c'] == 2
        test.emit(kitty('a=d,d=f,i=1,r=0'))
        assert [f[1] for f in frames()] == [green + blue * 3, blue + b'\0' * 12]
        test.emit(kitty('a=d,d=f,i=1,r=999') + kitty('a=d,d=f,i=1,r=1'))
        assert len(frames()) == 1
        test.emit(kitty('a=d,d=F,i=1,r=0'))
        assert capture() == b''
        print('PASS compose validation and Ghostty frame deletion numbering')

        root()
        test.emit(kitty('a=f,i=1,f=32,s=2,v=2,z=-1,X=1', green * 4) +
            kitty('a=a,i=1,r=1,z=80,c=1,s=3,v=2'))
        started = time.monotonic()
        test.wait(lambda: time.monotonic() - started >= 0.09)
        parked = state()
        assert parked['c'] == 1 and parked['s'] == 1, ('trailing gapless frame displayed', parked)
        test.emit(kitty('a=a,i=1,c=1,s=2,v=1'))
        started = time.monotonic()
        test.wait(lambda: time.monotonic() - started >= 0.09)
        parked = state()
        assert parked['c'] == 1 and parked['s'] == 2, parked
        print('PASS trailing gapless frames stay hidden at loading and finite-loop boundaries')

        root()
        test.emit(kitty('a=f,i=1,f=32,s=2,v=2,z=1000,X=1', green * 4) +
            kitty('a=a,i=1,r=1,z=80,s=3,v=1'))
        started = time.monotonic()
        test.wait(lambda: time.monotonic() - started >= 0.10)
        test.emit(kitty('a=a,i=1,s=1'))
        stopped = state()
        assert stopped['c'] == 2 and stopped['s'] == 1, ('stop lost elapsed frames', stopped)
        test.emit(kitty('a=a,i=1,c=1,r=1,z=200,s=3'))
        started = time.monotonic()
        test.wait(lambda: time.monotonic() - started >= 0.12)
        test.emit(kitty('a=c,i=1,r=2,c=1,C=1'))
        started = time.monotonic()
        test.wait(lambda: time.monotonic() - started >= 0.11)
        assert state()['c'] == 2, 'composition restarted the displayed frame gap'
        test.emit(kitty('a=a,i=1,c=1,r=1,z=80,s=3,v=1'))
        test.emit(kitty('a=a,i=1,r=2,z=80'))
        started = time.monotonic()
        test.wait(lambda: time.monotonic() - started >= 0.35)
        test.emit(kitty('a=a,i=1,v=2'))
        exhausted = state()
        assert exhausted['s'] == 1 and exhausted['v'] == 1, ('lowered loop budget underflow', exhausted)
        print('PASS control catches up before changes, composition keeps its timer and loop budgets saturate')

        root()
        test.emit(kitty('a=f,i=1,f=32,s=2,v=2,z=80,X=1', green * 4) +
            kitty('a=f,i=1,f=32,s=2,v=2,z=80,X=1', blue * 4))
        before = os.path.getsize(test.replies)
        test.emit(kitty('a=a,i=1,r=1,z=80,c=1,s=3,v=2,q=0'))
        started = time.monotonic()
        test.wait(lambda: time.monotonic() - started >= 0.25)
        exhausted = state()
        assert exhausted['s'] == 1, 'one lazy capture did not catch up elapsed frames'
        assert exhausted['c'] == 3 and exhausted['v'] == 1
        assert os.path.getsize(test.replies) == before, 'successful animation control replied'
        test.emit(kitty('a=a,i=1,c=1,s=2,v=1'))
        test.wait(lambda: state()['c'] == 3)
        assert state()['s'] == 2
        test.emit(kitty('a=a,i=1,c=2,s=1'))
        stopped = state()
        assert stopped['c'] == 2 and stopped['s'] == 1
        test.emit(kitty('a=a,i=1,r=1,z=-1') + kitty('a=a,i=1,r=2,z=-1,c=1,s=3,v=2'))
        test.wait(lambda: state()['s'] == 1)
        assert state()['c'] == 3
        print('PASS elapsed playback, finite loops, loading parking, stop and gapless skip')

        client = test.attach(cw=1, ch=1)
        test.emit(kitty('a=a,i=1,c=1,s=1'))
        test.wait(lambda: any((255, 0, 0) in im[2] for im in client.images()))
        test.emit(kitty('a=a,i=1,c=2'))
        test.wait(lambda: any((0, 255, 0) in im[2] for im in client.images()))
        test.emit(kitty('a=c,i=1,r=3,c=2,C=1'))
        test.wait(lambda: any((0, 0, 255) in im[2] for im in client.images()))
        test.detach(client)
        print('PASS current frame and composition invalidate scheduled sixel draw')

        root()
        test.run('set-option', '-s', 'image-total-size-limit', '32')
        reply('a=f,i=1,f=32,s=2,v=2,X=1', green * 4, b'\033_Gi=1,r=2;OK\033\\')
        reply('a=f,i=1,f=32,s=2,v=2,X=1', green * 4, b'\033_Gi=1;ENOSPC\033\\')
        test.emit(kitty('a=d,d=f,i=1,r=2'))
        reply('a=f,i=1,f=32,s=2,v=2,X=1', blue * 4, b'\033_Gi=1,r=2;OK\033\\')
        root()
        assert len(frames()) == 1, 'retransmit kept animation frames'
        test.run('set-option', '-s', 'image-total-size-limit', '268435456')
        print('PASS frame quota, release and retransmit reset')

        name = objects.create(green * 4)
        test.emit(kitty('a=f,i=1,f=32,s=2,v=2,z=60,t=s,X=1', name.encode()))
        control = Control(test, 'ignore-size,image-shm')
        restored = control.exchange('capture-pane -K -p -t ' + pane)
        restored_frames = frames(restored, objects)
        assert len(restored_frames) == 2
        for index, (keys, value) in enumerate(restored_frames):
            assert b't=s,S=16,q=2' in keys, keys
            assert value == (red if index == 0 else green) * 4
        assert b'a=f,' in restored and b'a=a,' in restored
        control.close()
        control = None
        print('PASS shared-memory append and frame restore objects')

        test.emit(kitty('a=d,d=A'))
        large = red * (1024 * 1024)
        for action, colour in [('T', large), ('f', green * (1024 * 1024)), ('f', blue * (1024 * 1024))]:
            name = objects.create(colour)
            test.emit(kitty('a=%s,i=1,f=32,s=1024,v=1024,c=1,r=1,C=1,t=s,X=1' % action if action == 'T' else
                'a=f,i=1,f=32,s=1024,v=1024,t=s,X=1', name.encode()))
        first = capture()
        first_token = token(first)
        assert first_token.split(':')[1] == '1', first_token
        second = capture('-k', first_token)
        token2 = token(second)
        assert token2.split(':')[1] == '2', token2
        final = capture('-k', token2)
        assert b'a=a,' in final and b'more ' not in final
        merged = frames(first + second + final)
        assert [f[1] for f in merged] == [large, green * (1024 * 1024), blue * (1024 * 1024)]
        for change, current in [(kitty('a=f,i=1,r=2,f=32,s=1,v=1,X=1', blue), first_token),
                (kitty('a=d,d=f,i=1,r=1'), token2)]:
            test.emit(change)
            result = capture('-k', current, check=False)
            assert result.returncode != 0 and b'image changed' in result.stderr, result
        print('PASS cross-frame budget continuation and edit/delete rejection')
    finally:
        if control is not None:
            control.close()
        objects.close()
PY
