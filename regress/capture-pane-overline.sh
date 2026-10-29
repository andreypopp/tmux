#!/bin/sh

set -eu
TERM=screen
export TERM
. ./regress-common.inc
[ -n "${TEST_TMUX:-}" ] || TEST_TMUX=$(readlink -f ../tmux)
TMP=$(test_tmpdir)
TMUX="$TEST_TMUX -S$TMP/socket -f/dev/null"
trap 'stop_server $TMUX; rm -rf "$TMP"' 0 1 15
$TMUX new-session -d -s overline -x 12 -y 4
$TMUX set-option -g remain-on-exit on
$TMUX set-option -g remain-on-exit-format ''
printf '\033[53mA' >"$TMP/input"
pane=$($TMUX new-window -d -P -F '#{pane_id}' "cat '$TMP/input'")
wait_for_output 1 $TMUX display-message -p -t "$pane" '#{pane_dead}'
$TMUX capture-pane -p -e -T -t "$pane" -S 0 -E 0 >"$TMP/actual"
printf '\033[53mA\n' >"$TMP/expected"
if ! cmp "$TMP/expected" "$TMP/actual"; then
  echo 'FAIL overline SGR parameter'; exit 1
fi
echo 'PASS overline SGR parameter'
