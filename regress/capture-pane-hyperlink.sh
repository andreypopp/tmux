#!/bin/sh

# capture-pane -e for OSC 8 hyperlink

PATH=/bin:/usr/bin
TERM=screen

. ./regress-common.inc

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -LtestA$$ -f/dev/null"
TMP=$(mktemp)
TMP2=$(mktemp)
TMP3=$(mktemp)
trap 'stop_server $TMUX; rm -f "$TMP" "$TMP2" "$TMP3"' 0 1 15
$TMUX kill-server 2>/dev/null

do_test() {
  $TMUX -f/dev/null new -d "
  printf '$1'
  $TMUX capturep -peS0 -E1 >$TMP"
  printf "$2\n" > $TMP2
  sleep 1
  cmp $TMP $TMP2 || exit 1
  return 0
}

do_test '\033]8;id=1;https://github.com\033\\test1\033]8;;\033\\\n' '\033]8;id=1;https://github.com\033\\test1\033]8;;\033\\\n' || exit 1
do_test '\033]8;;https://github.com/tmux/tmux\033\\test1\033]8;;\033\\\n' '\033]8;;https://github.com/tmux/tmux\033\\test1\033]8;;\033\\\n' || exit 1

$TMUX has 2>/dev/null && exit 1

printf '\033]8;id=1;https://example.com\033\\abcdefghijklmnop\033]8;;\033\\' >"$TMP3"
$TMUX new -d -x 10 -y 4 "cat '$TMP3'; $TMUX wait-for hold"
for join in '' J; do
  if [ "$join" = J ]; then
    separator=''
  else
    separator='\n'
  fi
  printf '\033]8;id=1;https://example.com\033\\abcdefghij\033]8;;\033\\%b\033]8;id=1;https://example.com\033\\klmnop\033]8;;\033\\\n' "$separator" >"$TMP2"
  wait_until capture_matches_file "$TMP2" "$TMP" \
    $TMUX capture-pane -pe${join} -S 0 -E 1 || exit 1
  echo "PASS continuation link -e${join}"
done

exit 0
