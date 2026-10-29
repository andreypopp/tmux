#!/bin/sh

set -eu
TERM=screen
export TERM
. ./regress-common.inc
[ -n "${TEST_TMUX:-}" ] || TEST_TMUX=$(readlink -f ../tmux)
TMP=$(test_tmpdir)
TMUX="$TEST_TMUX -S$TMP/socket -f/dev/null"
trap 'stop_server $TMUX; rm -rf "$TMP"' 0 1 15
command="stty -opost -echo; cat '$TMP/input'; read line;
  printf '\033[?1049l\033]2;restored\007'; exec sleep 60"
printf '%b' \
  'abcdefghijklmnop\r\n\r\n\r\n\r\n\033[1;1Habc\033[48;2;223;23' \
  '1;236m\033[K\033[m\033[?1049hALT' >"$TMP/input"
$TMUX new-session -d -s history -x 12 -y 4 "$command"
wait_for_text ALT $TMUX capture-pane -p -T -t history
$TMUX resize-window -t history -x 7 -y 4
$TMUX capture-pane -p -L -F -T -t history -S -2 -E -1 >"$TMP/actual"
printf '%s\n' '-2 W abcdefghijkl' '-1 - mnop' >"$TMP/expected"
if ! diff -u "$TMP/expected" "$TMP/actual"; then
  echo 'FAIL alternate-screen history width'
  exit 1
fi
echo 'PASS alternate-screen history width'

$TMUX capture-pane -p -r -t history -S - -E -1 >"$TMP/history"
printf 'abcdefghijklmnop\033[K\r\n' >"$TMP/expected"
cmp "$TMP/expected" "$TMP/history"
$TMUX capture-pane -p -ar -t history >"$TMP/screen"
$TMUX capture-pane -p -ar -t history -S - -E - >"$TMP/main"
cat "$TMP/history" "$TMP/screen" >"$TMP/joined"
cmp "$TMP/main" "$TMP/joined"
$TMUX capture-pane -p -r -t history -S -3 -E -2 >"$TMP/chunks"
$TMUX capture-pane -p -r -t history -S -1 -E -1 >>"$TMP/chunks"
cmp "$TMP/history" "$TMP/chunks"
$TMUX capture-pane -p -ar -t history -S 0 -E 0 >"$TMP/chunks"
$TMUX capture-pane -p -ar -t history -S 1 -E - >>"$TMP/chunks"
cmp "$TMP/screen" "$TMP/chunks"
echo 'PASS reflowed alternate-screen ranges concatenate'

$TMUX new-session -d -s target -x 7 -y 4 \
  "stty -opost; cat '$TMP/main';
  printf '\033]2;replayed\007'; exec sleep 60"
wait_for_output replayed $TMUX display-message -p -t target '#{pane_title}'
$TMUX capture-pane -p -r -t target -S - -E - >"$TMP/target"
cmp "$TMP/main" "$TMP/target"
$TMUX send-keys -t history Enter
wait_for_output restored $TMUX display-message -p -t history '#{pane_title}'
$TMUX capture-pane -p -r -t history -S - -E - >"$TMP/restored"
cmp "$TMP/main" "$TMP/restored"
echo 'PASS alternate-screen replay matches main after exit at current width'

printf '%b' \
  '123456789012xyzABCDEFGHI\r\n\r\n\r\n\033[?1049h\033[H\033[X' \
  '\033[2;1HALT' >"$TMP/input"
$TMUX new-session -d -s seam -x 12 -y 4 "$command"
wait_for_text ALT $TMUX capture-pane -p -T -t seam
$TMUX resize-window -t seam -x 7 -y 4
$TMUX capture-pane -p -r -t seam -S - -E -1 >"$TMP/history"
printf '123456789012\033[K\r\nxyzABCD' >"$TMP/expected"
cmp "$TMP/expected" "$TMP/history"
$TMUX capture-pane -p -ar -t seam >"$TMP/screen"
cat "$TMP/history" "$TMP/screen" >"$TMP/joined"
$TMUX capture-pane -p -ar -t seam -S - -E - >"$TMP/main"
cmp "$TMP/main" "$TMP/joined"
$TMUX send-keys -t seam Enter
wait_for_output restored $TMUX display-message -p -t seam '#{pane_title}'
$TMUX capture-pane -p -r -t seam -S - -E - >"$TMP/restored"
cmp "$TMP/main" "$TMP/restored"
echo 'PASS last history row wraps into saved screen, not alternate screen'

printf '%b' \
  '\033_Ga=T,f=24,s=1,v=1,i=42,c=2,r=3,C=1,q=2;AAAA\033\\abcdef' \
  'ghijklmnop\r\n\r\n\r\n\r\n\033[?1049hALT' >"$TMP/input"
$TMUX new-session -d -s images -x 12 -y 4 "$command"
wait_for_text ALT $TMUX capture-pane -p -T -t images
$TMUX resize-window -t images -x 7 -y 4
$TMUX capture-pane -p -rG -t images -S - -E -1 >"$TMP/history"
$TMUX capture-pane -p -arG -t images >"$TMP/screen"
$TMUX capture-pane -p -arG -t images -S - -E - >"$TMP/main"
cat "$TMP/history" "$TMP/screen" >"$TMP/joined"
cmp "$TMP/main" "$TMP/joined"
grep -Fq "$(printf '\033_Ga=p,i=42,')" "$TMP/main"
$TMUX send-keys -t images Enter
wait_for_output restored $TMUX display-message -p -t images '#{pane_title}'
$TMUX capture-pane -p -rG -t images -S - -E - >"$TMP/restored"
cmp "$TMP/main" "$TMP/restored"
echo 'PASS alternate-screen image spans match main after exit'
