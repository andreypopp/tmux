#!/bin/sh
set -eu
DIR=$(mktemp -d "${REGRESS_ROOT:-/tmp}/query-routing-XXXXXX")
[ -n "${TEST_TMUX:-}" ] || TEST_TMUX=$(cd .. && pwd)/tmux
export DIR TEST_TMUX
trap '"$TEST_TMUX" -S "$DIR/socket" kill-server 2>/dev/null || :; rm -rf "$DIR"' 0 1 2 15
python3 -u - <<'PY'
import os,select,shlex,signal,subprocess,sys,time
signal.alarm(120)
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
def send(data):
    end=time.monotonic()+5
    while data:
        assert time.monotonic()<end,'terminal input write'
        if select.select([],[fd],[],.01)[1]:
            try: data=data[os.write(fd,data):]
            except BlockingIOError: pass
def wait(fn, label, timeout=5):
    end=time.monotonic()+timeout
    while not fn():
        assert time.monotonic()<end,label
        select.select([],[],[],.01)
def drain():
    end=time.monotonic()+5
    while select.select([fd],[],[],.02)[0]:
        data=os.read(fd,65536)
        for number in (10,11):
            if b'\x1b]%d;?' % number in data:
                send(b'\x1b]%d;rgb:0000/0000/0000\x07' % number)
        assert time.monotonic()<end
def query(request, owner=writer):
    drain()
    wire=b'\x1bPtmux;'+request.replace(b'\x1b',b'\x1b\x1b')+b'\x1b\\'
    owner.write(wire.hex()+'\n'); read_until(request)
def received(path, want, label):
    def matches():
        try: got=open(path,'rb').read()
        except FileNotFoundError: got=b''
        assert len(got)<=len(want),(label,'extra bytes',got[-80:])
        return got==want
    wait(matches,label)
def active_text(): return cmd('capture-pane','-p','-t',active)
def clear_active():
    cmd('send-keys','-t',active,'-R'); cmd('clear-history','-t',active)
def visible(reply):
    return b''.join(b'^[' if c==27 else b'^G' if c==7 else bytes([c]) for c in reply)
expected=b''
try:
    read_until(b'\x1b[c')
    # Complete the one-shot startup replies before testing pane ownership.
    send(b'\x1b[8;24;80t\x1b[4;480;640t\x1b[?1;2c\x1b[>0;1;0c\x1bP>|startup\x1b\\\x1b[?997;1n\x1b[?2026;2$y\x1b]10;rgb:0000/0000/0000\x07\x1b]11;rgb:0000/0000/0000\x07')
    active=cmd('split-window','-t',pane,'-P','-F','#{pane_id}',"printf '\\033[?2004h'; stty raw -echo; exec cat -v").decode()
    wait(lambda:cmd('list-clients','-F','#{client_name}'),'client attach')
    send(b'\x1b[1;2R')
    wait(lambda:cmd('show-option','-gv','@f3')==b'yes','unmatched S-F3 binding')
    query(b'\x1b[?2004$p'); expires=time.monotonic()+5.1
    review_cases=[
        ('long-reply',b'\x1b]52;c;?\x07',[b'\x1b]52;c;'+b'YQ=='*2048+b'\x07']),
        ('long-request',b'\x1b_Ga=T,i=77;'+b'A'*8192+b'\x1b\\',[b'\x1b_Gi=77;OK\x1b\\']),
        ('palette-list',b'\x1b]4;1;?;2;?\x07',[b'\x1b]4;2;rgb:aaaa/bbbb/cccc\x07',b'\x1b]4;1;rgb:aaaa/bbbb/cccc\x07']),
        ('dynamic-list',b'\x1b]10;?;?\x07',[b'\x1b]11;rgb:aaaa/bbbb/cccc\x07',b'\x1b]10;rgb:aaaa/bbbb/cccc\x07']),
        ('mixed-palette',b'\x1b]4;1;red;2;?\x07',[b'\x1b]4;2;rgb:aaaa/bbbb/cccc\x07']),
        ('cap-list',b'\x1bP+q436f;544e\x1b\\',[b'\x1bP1+r436f=323536;544e=787465726d\x1b\\',b'\x1bP1+r544e=787465726d\x1b\\']),
        ('res-list',b'\x1bP+Q436f;544e\x1b\\',[b'\x1bP1+R544e=787465726d\x1b\\',b'\x1bP1+R436f=323536\x1b\\']),
        ('invalid-cap',b'\x1bP+q436f;544e\x1b\\',[b'\x1bP0+r\x1b\\',b'\x1bP1+r544e=787465726d\x1b\\'])]
    for name,request,replies in review_cases:
        if os.environ.get('QUERY_REVIEW') and name!=os.environ['QUERY_REVIEW']:continue
        query(request)
        for reply in replies:
            send(reply); expected+=reply
            received(output,expected,name+' owner')
            assert active_text()==b'',(name,'reply leaked',active_text())
        print(name+': PASS')
    if not os.environ.get('QUERY_REVIEW') or os.environ['QUERY_REVIEW']=='paste':
        query(b'\x1b[6n')
        send(b'\x1b[200~\x1b[3;4R\x1b[201~paste-barrier')
        wait(lambda:b'paste-barrier' in active_text(),'paste input barrier')
        assert b'^[[3;4R' in active_text(),('pasted reply missing',active_text())
        received(output,expected,'paste must not reach query owner')
        clear_active(); send(b'\x1b[3;4R'); expected+=b'\x1b[3;4R'
        received(output,expected,'real CPR after paste')
        print('paste: PASS')
    if not os.environ.get('QUERY_REVIEW') or os.environ['QUERY_REVIEW']=='paste-partial':
        query(b'\x1b[6n')
        send(b'\x1b[200~partial:\x1b[3')
        wait(lambda:b'partial:^[[3' in active_text(),'partial paste delayed by query',.25)
        send(b';4R\x1b[201~paste-barrier')
        wait(lambda:b'paste-barrier' in active_text(),'partial paste input barrier')
        received(output,expected,'partial paste must not reach query owner')
        clear_active(); send(b'\x1b[3;4R'); expected+=b'\x1b[3;4R'
        received(output,expected,'real CPR after partial paste')
        print('paste-partial: PASS')
    if os.environ.get('QUERY_REVIEW'):sys.exit(0)
    cases=[
        ('APC', b'\x1b_Ga=T,i=77;AAAA\x1b\\',b'\x1b_Gi=77;OK\x1b\\'),
        ('APC-query', b'\x1b_Ga=q;AAAA\x1b\\',b'\x1b_G;OK\x1b\\'),
        ('CPR',b'\x1b[6n',b'\x1b[3;4R'),
        ('DECXCPR',b'\x1b[?6n',b'\x1b[?3;4;1R'),
        ('DSR',b'\x1b[5n',b'\x1b[0n'),
        ('private-DSR',b'\x1b[?15n',b'\x1b[?13n'),
        ('DECRQM',b'\x1b[4$p',b'\x1b[4;2$y'),
        ('private-DECRQM',b'\x1b[?25$p',b'\x1b[?25;1$y'),
        ('kitty-keyboard',b'\x1b[?u',b'\x1b[?1u'),
        ('XTSMGRAPHICS',b'\x1b[?1;1;0S',b'\x1b[?1;0;256S'),
        ('XTQMODKEYS',b'\x1b[>4m',b'\x1b[>4;2m'),
        ('XTQFMTKEYS',b'\x1b[>0f',b'\x1b[>0;1f'),
        ('XTVERSION',b'\x1b[>q',b'\x1bP>|terminal\x1b\\'),
        ('DA3',b'\x1b[=c',b'\x1bP!|1234\x1b\\'),
        ('theme',b'\x1b[?996n',b'\x1b[?997;2n'),
        ('DA1',b'\x1b[c',b'\x1b[?1;2c'),
        ('DA2',b'\x1b[>c',b'\x1b[>0;1;0c'),
        ('XTWINOPS',b'\x1b[18t',b'\x1b[8;24;80t'),
        ('XTWINOPS-state',b'\x1b[11t',b'\x1b[2t'),
        ('XTWINOPS-label',b'\x1b[20t',b'\x1b]Llabel\x1b\\'),
        ('XTWINOPS-title',b'\x1b[21t',b'\x1b]ltitle\x1b\\'),
        ('DECREQTPARM',b'\x1b[0x',b'\x1b[2;1;1;112;112;1;0x'),
        ('DECRQDE',b'\x1b["v',b'\x1b[24;80"w'),
        ('DECRQLP',b"\x1b[1'|",b'\x1b[1;1;1&w'),
        ('XTREPORTSGR',b'\x1b[1;1;2;2#|',b'\x1b[1;31m'),
        ('DECMSR',b'\x1b[?62n',b'\x1b[12*{'),
        ('DECRQCRA',b'\x1b[7;1;1;1;2;2*y',b'\x1bP7!~1234\x1b\\'),
        ('DECCKSR',b'\x1b[?63;8n',b'\x1bP8!~4321\x1b\\'),
        ('DECRQPSR',b'\x1b[1$w',b'\x1bP1$u1;1;1\x1b\\'),
        ('DECRQUPSS',b'\x1b[&u',b'\x1bP0!u%5\x1b\\'),
        ('DECRQSS',b'\x1bP$qm\x1b\\',b'\x1bP1$r0m\x1b\\'),
        ('XTGETTCAP',b'\x1bP+q544e\x1b\\',b'\x1bP1+r544e=787465726d\x1b\\'),
        ('XTGETXRES',b'\x1bP+Q666f6f\x1b\\',b'\x1bP0+R666f6f\x1b\\'),
        ('OSC-built-in-colour',b'\x1b]10;?\x07',b'\x1b]10;rgb:aaaa/bbbb/cccc\x07'),
        ('OSC-colour',b'\x1b]12;?\x07',b'\x1b]12;rgb:aaaa/bbbb/cccc\x07'),
        ('OSC-palette',b'\x1b]4;99;?\x07',b'\x1b]4;99;rgb:aaaa/bbbb/cccc\x07'),
        ('OSC-special-colour',b'\x1b]5;0;?\x07',b'\x1b]5;0;rgb:aaaa/bbbb/cccc\x07'),
        ('OSC-clipboard',b'\x1b]52;c;?\x07',b'\x1b]52;c;YWJj\x07'),
        ('OSC-kitty-colour',b'\x1b]21;foreground=?\x1b\\',b'\x1b]21;foreground=rgb:aaaa/bbbb/cccc\x1b\\'),
        ('OSC-program-status',b'\x1b]7501;?\x1b\\',b'\x1b]7501;ok\x1b\\'),
        ('OSC-allowed',b'\x1b]60\x1b\\',b'\x1b]60;allowColorOps\x1b\\'),
        ('OSC-disallowed',b'\x1b]61;1\x1b\\',b'\x1b]61;GetSelection\x1b\\'),
        ('OSC-query-allowed',b'\x1b]62;1\x1b\\',b'\x1b]62;GetSelection\x1b\\'),
        ('OSC-cell-size',b'\x1b]1337;ReportCellSize\x07',b'\x1b]1337;ReportCellSize=16;8;1\x07'),
        ('OSC-variable',b'\x1b]1337;ReportVariable=YWJj\x07',b'\x1b]1337;ReportVariable=ZGVm\x07')]
    upstream={'theme','OSC-clipboard','OSC-palette','OSC-built-in-colour'}
    for name,request,reply in cases:
        if os.environ.get('QUERY_CASE') and name != os.environ['QUERY_CASE']:continue
        query(request)
        send(reply); expected+=reply
        received(output,expected,name+' owner')
        assert active_text()==b'', (name,'reply leaked',active_text())
        # No request: the same frame follows upstream, not a stale owner.
        send(reply)
        if name not in upstream:
            wait(lambda:visible(reply) in active_text().replace(b'\n',b''),name+' unmatched reply')
            clear_active()
        else:
            # Upstream consumes these replies: neither pane must receive them.
            # A plain key is the input-order barrier.
            send(b'barrier')
            wait(lambda:b'barrier' in active_text(),name+' upstream barrier')
            assert active_text()==b'barrier',(name,'upstream reply leaked',active_text())
            clear_active()
        assert open(output,'rb').read()==expected,(name,'stale query captured reply')
        print(name+': PASS (owner and unmatched control)')
    if os.environ.get('QUERY_CASE'):sys.exit(0)
    cmd('set-option','-s','escape-time','10')
    request=b'\x1b[?25$p'; reply=b'\x1b[?25;1$y'
    query(request)
    send(b'\x1b'+reply[:-3])
    select.select([],[],[],.02)
    send(reply[-3:])
    expected+=reply
    received(output,expected,'split reply at escape-time=10')
    query(request)
    cmd('set-option','-g','@escape','no')
    drain()
    started=time.monotonic(); send(b'\x1b')
    wait(lambda:cmd('show-option','-gv','@escape')==b'yes','lone Escape delayed by pending query',.25)
    assert time.monotonic()-started<.25, 'lone Escape delayed by pending query'
    send(reply)
    expected+=reply
    received(output,expected,'reply after lone Escape missed its owner')
    print('lone Escape with pending query: PASS (%.3fs)' % (time.monotonic()-started))
    sidefifo=root+'/side-query'; sideoutput=root+'/side-reply'; os.mkfifo(sidefifo)
    sidecommand=' '.join(map(shlex.quote,[sys.executable,'-u',code,sidefifo,sideoutput]))
    cmd('set-option','-g','side-status','left')
    cmd('set-option','-g','side-status-command',sidecommand)
    sidewriter=open(sidefifo,'w',buffering=1)
    request=b'\x1b[?2026$p'; reply=b'\x1b[?2026;2$y'
    query(request,sidewriter); send(reply)
    received(sideoutput,reply,'side sync reply')
    assert active_text()==b'', 'side reply leaked to active pane'
    sideexpected=reply
    while time.monotonic()<expires:
        select.select([],[],[],min(.05,expires-time.monotonic()))
    request=b'\x1b[?2004$p'; reply=b'\x1b[?2004;1$y'
    query(request,sidewriter); send(reply); sideexpected+=reply
    received(sideoutput,sideexpected,'expired pane query before fresh side query')
    assert open(output,'rb').read()==expected, 'reply went to expired owner'
    print('expired query before fresh same-kind/id owner: PASS')
    def side_receive(reply, label):
        global sideexpected
        send(reply); sideexpected+=reply
        received(sideoutput,sideexpected,label)
        assert open(output,'rb').read()==expected,(label,'reply reached pane')
    query(b'\x1b[?u',sidewriter)
    side_receive(b'\x1b[?1u','side keyboard')
    print('side keyboard: PASS')
    # A silent family does not block a different key, and DA is not a sentinel.
    query(b'\x1b[?u'); query(b'\x1b[c')
    send(b'\x1b[?1;2c'); expected+=b'\x1b[?1;2c'
    received(output,expected,'silent keyboard then DA')
    query(b'\x1b[?u',sidewriter)
    send(b'\x1b[?1u'); expected+=b'\x1b[?1u'
    received(output,expected,'oldest silent keyboard owns first reply')
    side_receive(b'\x1b[?2u','newer keyboard owns second reply')
    print('silent keyboard followed by DA: PASS (no sentinel)')
    for reverse in (False,True):
        query(b'\x1b[?25$p'); query(b'\x1b[?2026$p',sidewriter)
        if reverse:side_receive(b'\x1b[?2026;2$y','reverse side mode')
        send(b'\x1b[?25;1$y'); expected+=b'\x1b[?25;1$y'
        received(output,expected,'interleaved pane mode')
        if not reverse:side_receive(b'\x1b[?2026;2$y','ordered side mode')
    query(b'\x1b[6n'); query(b'\x1b[6n',sidewriter)
    send(b'\x1b[3;4R'); expected+=b'\x1b[3;4R'
    received(output,expected,'first CPR owner')
    side_receive(b'\x1b[5;6R','second CPR owner')
    print('interleaved keys and same-key FIFO: PASS')
    for request,key,reply in ((b'\x1b[?u',b'\x1b[97;5u',b'\x1b[?1u'),
                              (b'\x1b[6n',b'\x1b[A',b'\x1b[3;4R')):
        query(request); send(key+b'key-barrier')
        wait(lambda:b'key-barrier' in active_text(),'key input barrier')
        assert (b'^A' if key.endswith(b'u') else b'^[[A') in active_text(),('key missing',key,active_text())
        assert open(output,'rb').read()==expected,('key captured by query',key)
        clear_active(); send(reply); expected+=reply
        received(output,expected,'reply after key collision control')
    print('kitty key and arrow with query pending: PASS')
    assert active_text()==b'', 'reply leaked to active pane'
    sidewriter.close()
    print('startup replies and pane DA1 ownership: PASS')
    print('query routing: PASS (all frame families, inactive owner, unmatched controls, split reply, lone Escape, modified F3, side job, FIFO)')
finally:
    cmd('kill-server')
    writer.close(); os.close(fd)
    wait(lambda:os.waitpid(pid,os.WNOHANG)[0],'attached client exit')
PY
