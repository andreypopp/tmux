#!/bin/sh

set -eu
TERM=screen
export TERM
. ./regress-common.inc
[ -n "${TEST_TMUX:-}" ] || TEST_TMUX=$(readlink -f ../tmux)
TMP=$(test_tmpdir)
TMUX="$TEST_TMUX -S$TMP/socket -f/dev/null"
trap 'stop_server $TMUX; rm -rf "$TMP"' 0 1 15
$TMUX new-session -d -s clear -x 12 -y 7
$TMUX set-option -g remain-on-exit on
$TMUX set-option -g remain-on-exit-format ''
printf '%b' \
  '\033[48;2;223;231;236m\033[K\033[2;1H界\r\033[K\033[3;1H\033[' \
  '12X\033[4;1H\033[J\033[5;1HX\033[6;1H\t\033[7;1H\033[mabcdef' \
  'ghijkl\r界ab\r\033[2X' >"$TMP/input"
pane=$($TMUX new-window -d -P -F '#{pane_id}' "stty -opost; cat '$TMP/input'")
wait_for_output 1 $TMUX display-message -p -t "$pane" '#{pane_dead}'
$TMUX capture-pane -p -R -t "$pane" >"$TMP/grid"
awk '
  /C [0-6],/ {
    split($2, p, ",")
    if ($0 ~ /flags=[^ ]*CLEARED/) c = "c"
    else if ($0 ~ /flags=[^ ]*TAB/) c = "t"
    else if ($0 ~ /flags=[^ ]*PADDING/) c = "p"
    else c = "w"
    r[p[1]] = r[p[1]] c
  }
  END { for (y = 0; y < 7; y++) print y, r[y] }
' "$TMP/grid" >"$TMP/actual"
cat >"$TMP/expected" <<'EOF'
0 cccccccccccc
1 cccccccccccc
2 cccccccccccc
3 cccccccccccc
4 wccccccccccc
5 tpppppppcccc
6 ccwwwwwwwwww
EOF
if ! diff -u "$TMP/expected" "$TMP/actual"; then
  echo 'FAIL RGB erase cell flags'; exit 1
fi
echo 'PASS RGB erase cell flags'
