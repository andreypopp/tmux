#!/bin/sh

set -eu
TERM=screen
export TERM
. ./regress-common.inc
[ -n "${TEST_TMUX:-}" ] || TEST_TMUX=$(readlink -f ../tmux)
TMP=$(test_tmpdir)
TMUX="$TEST_TMUX -S$TMP/socket -f/dev/null"
trap 'stop_server $TMUX; rm -rf "$TMP"' 0 1 15
printf 'abcdefghijklmnop\r\n\r\n\r\n\033[?1049hALT' >"$TMP/input"
$TMUX new-session -d -s history -x 12 -y 3 \
  "stty -opost; cat '$TMP/input'; exec sleep 60"
wait_for_text ALT $TMUX capture-pane -p -T
$TMUX resize-window -t history -x 7 -y 3
$TMUX capture-pane -p -L -F -T -S -2 -E -1 >"$TMP/actual"
printf '%s\n' '-2 W abcdefghijkl' '-1 - mnop' >"$TMP/expected"
if ! diff -u "$TMP/expected" "$TMP/actual"; then
  echo 'FAIL alternate-screen history width'
  exit 1
fi
echo 'PASS alternate-screen history width'
