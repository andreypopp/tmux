#!/bin/sh

# A side status job whose output scrolls the side status line must not crash
# the server. The job prints more lines than the line has rows.

PATH=/bin:/usr/bin:/usr/local/bin
TERM=screen
export TERM

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Ltest$$ -f/dev/null"
trap '$TMUX kill-server 2>/dev/null' 0 1 15

$TMUX new-session -d -x 80 -y 24 'sleep 1000' || exit 1
$TMUX set-option -g side-status left || exit 1
$TMUX set-option -g side-status-width 20 || exit 1
$TMUX set-option -g side-status-command \
    'i=0; while [ $i -lt 200 ]; do echo line $i; i=$((i + 1)); done; exec sleep 1000' ||
    exit 1

python3 - "$TEST_TMUX" "test$$" <<'EOF2' || exit 1
import fcntl, os, pty, select, signal, struct, sys, termios, time

tmux = [sys.argv[1], "-L" + sys.argv[2], "-f/dev/null"]

master, slave = pty.openpty()
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
pid = os.fork()
if pid == 0:
    os.setsid()
    fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
    for fd in (0, 1, 2):
        os.dup2(slave, fd)
    os.environ["TERM"] = "screen"
    os.execvp(tmux[0], tmux + ["attach"])
os.close(slave)
os.set_blocking(master, False)

output = b""
deadline = time.monotonic() + 10
try:
    while b"line 199" not in output:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            print("did not see line 199")
            raise SystemExit(1)
        if not select.select([master], [], [], remaining)[0]:
            print("did not see line 199")
            raise SystemExit(1)
        chunk = os.read(master, 65536)
        if not chunk:
            print("the server exited")
            raise SystemExit(1)
        output += chunk
finally:
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
EOF2

$TMUX display-message -p '#{pid}' >/dev/null || {
    echo "the server exited while the side status job scrolled" >&2
    exit 1
}

exit 0
