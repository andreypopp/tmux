#!/bin/sh
set -eu
DIR=$(mktemp -d "${REGRESS_ROOT:-/tmp}/query-routing-XXXXXX")
[ -n "${TEST_TMUX:-}" ] || TEST_TMUX=$(cd .. && pwd)/tmux
export DIR TEST_TMUX
trap '"$TEST_TMUX" -S "$DIR/socket" kill-server 2>/dev/null || :; rm -rf "$DIR"' 0 1 2 15
python3 -u - <<'PY'
import os,select,shlex,signal,subprocess,sys,time
signal.alarm(60)
root=os.environ['DIR']; binary=os.environ['TEST_TMUX']
server=[binary,'-S',root+'/socket','-f','/dev/null']
def cmd(*a):
    return subprocess.check_output(server+list(a),timeout=5).strip()
fifo=root+'/query'; output=root+'/reply'; os.mkfifo(fifo)
code=root+'/pane.py'
with open(code,'w') as f:
    f.write('''import os,sys,threading,tty
tty.setraw(0)
def reader():
 with open(sys.argv[2],'ab',buffering=0) as out:
  while True: out.write(os.read(0,4096))
threading.Thread(target=reader,daemon=True).start()
for line in open(sys.argv[1]): os.write(1,bytes.fromhex(line.strip()))
''')
command=' '.join(map(shlex.quote,[sys.executable,'-u',code,fifo,output]))
pane=cmd('new-session','-d','-s','routing','-P','-F','#{pane_id}',command).decode()
writer=open(fifo,'w',buffering=1)
cmd('set-option','-g','allow-passthrough','all')
cmd('set-option','-g','status','off')
cmd('set-option','-g','assume-paste-time','0'); cmd('set-option','-s','escape-time','100')
cmd('bind-key','-n','Escape','set-option','-g','@escape','yes')
cmd('bind-key','-n','S-F3','set-option','-g','@f3','yes')
pid,fd=os.forkpty()
if pid==0:
    os.environ.pop('TMUX',None); os.environ.pop('TMUX_PANE',None)
    os.environ['TERM']='xterm-256color'
    os.execv(binary,server+['attach-session','-t','routing'])
os.set_blocking(fd,False)
def read_until(needle,timeout=5):
    data=b''; end=time.monotonic()+timeout
    while time.monotonic()<end:
        if select.select([fd],[],[],.05)[0]:
            data+=os.read(fd,65536)
            if needle in data:return data
    raise AssertionError(('missing query',needle,data[-1000:]))
try:
    read_until(b'\x1b[c')
    # Complete the one-shot startup replies before testing pane ownership.
    os.write(fd,b'\x1b[?1;2c\x1b[>0;1;0c\x1bP>|startup\x1b\\\x1b[?997;1n\x1b[?2026;2$y\x1b]10;rgb:0000/0000/0000\x07\x1b]11;rgb:0000/0000/0000\x07')
    active=cmd('split-window','-t',pane,'-P','-F','#{pane_id}','stty raw -echo; exec cat -v').decode()
    end=time.monotonic()+5
    while not cmd('list-clients','-F','#{client_name}'):
        assert time.monotonic()<end
    os.write(fd,b'\x1b[1;2R')
    end=time.monotonic()+5
    while cmd('show-option','-gv','@f3')!=b'yes':assert time.monotonic()<end
    cases=[
        (b'\x1b_Ga=q,i=77;AAAA\x1b\\',b'\x1b_Gi=77;OK\x1b\\'),
        (b'\x1b[6n',b'\x1b[1;2R'),
        (b'\x1b[4$p',b'\x1b[4;2$y'),
        (b'\x1b[?25$p',b'\x1b[?25;1$y'),
        (b'\x1b[?u',b'\x1b[?1u'),
        (b'\x1b[>q',b'\x1bP>|terminal\x1b\\'),
        (b'\x1b[?996n',b'\x1b[?997;2n'),
        (b'\x1b[c',b'\x1b[?1;2c'),
        (b'\x1b[>c',b'\x1b[>0;1;0c'),
        (b'\x1b[18t',b'\x1b[8;24;80t'),
        (b'\x1b]10;?\x07',b'\x1b]10;rgb:aaaa/bbbb/cccc\x07'),
        (b'\x1b]4;99;?\x07',b'\x1b]4;99;rgb:aaaa/bbbb/cccc\x07'),
        (b'\x1b]52;c;?\x07',b'\x1b]52;c;YWJj\x07')]
    os.write(fd,b'\x1b[8;24;80t\x1b[4;480;640t\x1b]10;rgb:0000/0000/0000\x07\x1b]11;rgb:0000/0000/0000\x07')
    expected=b''
    for request,reply in cases:
        end=time.monotonic()+5
        while select.select([fd],[],[],.1)[0]:
            os.read(fd,65536)
            assert time.monotonic()<end
        wire=b'\x1bPtmux;'+request.replace(b'\x1b',b'\x1b\x1b')+b'\x1b\\'
        writer.write(wire.hex()+'\n'); read_until(request)
        cmd('set-option','-g','@escape','no')
        os.write(fd,b'\x1b'+reply)
        expected+=reply; end=time.monotonic()+5
        while True:
            try: got=open(output,'rb').read()
            except FileNotFoundError:got=b''
            if got==expected and cmd('show-option','-gv','@escape')==b'yes':break
            assert time.monotonic()<end,(request,reply,expected,got)
            select.select([],[],[],.01)
    cmd('set-option','-s','escape-time','10')
    request=b'\x1b[?25$p'; reply=b'\x1b[?25;1$y'
    wire=b'\x1bPtmux;'+request.replace(b'\x1b',b'\x1b\x1b')+b'\x1b\\'
    writer.write(wire.hex()+'\n'); read_until(request)
    os.write(fd,b'\x1b'+reply[:-3])
    select.select([],[],[],.02)
    os.write(fd,reply[-3:])
    expected+=reply; end=time.monotonic()+5
    while True:
        got=open(output,'rb').read()
        if got==expected:break
        assert time.monotonic()<end,('split reply at escape-time=10',expected,got)
        select.select([],[],[],.01)
    sidefifo=root+'/side-query'; sideoutput=root+'/side-reply'; os.mkfifo(sidefifo)
    sidecommand=' '.join(map(shlex.quote,[sys.executable,'-u',code,sidefifo,sideoutput]))
    cmd('set-option','-g','side-status','left')
    cmd('set-option','-g','side-status-command',sidecommand)
    sidewriter=open(sidefifo,'w',buffering=1)
    end=time.monotonic()+5
    while select.select([fd],[],[],.1)[0]:
        os.read(fd,65536)
        assert time.monotonic()<end
    request=b'\x1b[?2026$p'; reply=b'\x1b[?2026;2$y'
    wire=b'\x1bPtmux;'+request.replace(b'\x1b',b'\x1b\x1b')+b'\x1b\\'
    sidewriter.write(wire.hex()+'\n'); read_until(request)
    os.write(fd,reply)
    end=time.monotonic()+5
    while True:
        try: got=open(sideoutput,'rb').read()
        except FileNotFoundError:got=b''
        if got==reply:break
        assert time.monotonic()<end,('side sync reply',got,cmd('capture-pane','-p','-t',active))
        select.select([],[],[],.01)
    assert cmd('capture-pane','-p','-t',active)==b'', 'side reply leaked to active pane'
    sideexpected=reply
    request=b'\x1b[?25$p'; reply=b'\x1b[?25;1$y'
    wire=b'\x1bPtmux;'+request.replace(b'\x1b',b'\x1b\x1b')+b'\x1b\\'
    writer.write(wire.hex()+'\n'); read_until(request)
    end=time.monotonic()+5.1
    while time.monotonic()<end:
        if select.select([fd],[],[],.05)[0]:os.read(fd,65536)
    sidewriter.write(wire.hex()+'\n'); read_until(request)
    os.write(fd,reply)
    sideexpected+=reply; end=time.monotonic()+5
    while True:
        got=open(sideoutput,'rb').read()
        if got==sideexpected:break
        assert time.monotonic()<end,('expired pane query before fresh side query',sideexpected,got)
        select.select([],[],[],.01)
    assert open(output,'rb').read()==expected, 'reply went to expired owner'
    print('expired query before fresh same-kind/id owner: PASS')
    sidewriter.close()
    print('query routing: PASS (all FIFO kinds, inactive owner, Escape before reply, split read at escape-time=10, unmatched modified F3, side-job sync)')
finally:
    cmd('kill-server')
    writer.close(); os.close(fd)
    end=time.monotonic()+5
    while True:
        if os.waitpid(pid,os.WNOHANG)[0]:break
        assert time.monotonic()<end
        select.select([],[],[],.01)
PY
