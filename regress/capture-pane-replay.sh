#!/bin/sh

set -eu
TERM=screen
export TERM
. ./regress-common.inc
[ -n "${TEST_TMUX:-}" ] || TEST_TMUX=$(readlink -f ../tmux)
TMP=$(test_tmpdir)
TMUX="$TEST_TMUX -S$TMP/socket -f/dev/null"
trap 'stop_server $TMUX; rm -rf "$TMP"' 0 1 15
$TMUX new-session -d -s replay -x 10 -y 12
$TMUX set-option -g remain-on-exit on
$TMUX set-option -g remain-on-exit-format ''
$TMUX set-option -g default-shell /bin/sh

pane() {
  $TMUX new-window -d -P -F '#{pane_id}' -t replay "stty -opost; cat '$1'"
}
wait_dead() {
  wait_for_output 1 $TMUX display-message -p -t "$1" '#{pane_dead}'
}
compare() {
  for flags in '-e -N' '-J' '' '-F' '-e -J'; do
    $TMUX capture-pane -p $flags -t "$source" \
      -S "$start" -E "$end" >"$TMP/source"
    $TMUX capture-pane -p $flags -t "$target" \
      -S "${target_start:-0}" -E "$rows" >"$TMP/target"
    cmp "$TMP/source" "$TMP/target" || { echo "FAIL $name ($flags)"; exit 1; }
  done
  echo "PASS $name"
}
roundtrip() {
  name=$1; start=$2; end=$3; rows=$4
  source=$(pane "$TMP/input")
  wait_dead "$source"
  $TMUX capture-pane -p -r -t "$source" -S "$start" -E "$end" >"$TMP/replay"
  target=$(pane "$TMP/replay")
  wait_dead "$target"
  compare
}

concatenate() {
  name=$1; first=$2; seam=$3; last=$4; flags=${5:-r}
  $TMUX capture-pane -p -"$flags" -t "$source" \
    -S "$first" -E "$seam" >"$TMP/chunks"
  $TMUX capture-pane -p -"$flags" -t "$source" \
    -S "$((seam + 1))" -E "$last" >>"$TMP/chunks"
  $TMUX capture-pane -p -"$flags" -t "$source" \
    -S "$first" -E "$last" >"$TMP/whole"
  cmp "$TMP/chunks" "$TMP/whole"
  echo "PASS concatenate $name seam"
}

printf '\033[48;2;223;231;236m\033[K' >"$TMP/input"
source=$(pane "$TMP/input")
wait_dead "$source"
$TMUX capture-pane -p -r -t "$source" -S 0 -E 0 >"$TMP/replay"
printf '\033[48;2;223;231;236m\033[10X\033[10C\033[m\r\n' >"$TMP/expected"
if ! cmp "$TMP/expected" "$TMP/replay"; then
  echo 'FAIL RGB erase replay'; exit 1
fi
echo 'PASS RGB erase replay'

printf '\033[1;31mA\033[48;2;223;231;236m\033[2X\033[2C\033[mZ' >"$TMP/input"
source=$(pane "$TMP/input")
wait_dead "$source"
$TMUX capture-pane -p -r -t "$source" -S 0 -E 0 >"$TMP/replay"
printf '%b' \
  '\033[1m\033[31mA\033[0m\033[48;2;223;231;236m\033[2X\033[2C' \
  '\033[mZ\033[K\r\n' >"$TMP/expected"
cmp "$TMP/expected" "$TMP/replay"
echo 'PASS erased run pen transitions'

printf '%b' \
  '\033[41mred\033[m\r\nplain  \r\n\033[42m\033[4X\033[4C\033[m' \
  'end\r\n1234567890next\r\n123456789界Z\r\n\033]8;id=test;https' \
  '://example.com\033\\abcdefghijklmnop\033]8;;\033\\' >"$TMP/input"
roundtrip cells 0 9 9

printf '%b' \
  '\033[1;3;4;38;2;12;34;56mstyled\033[m\r\n\016qq\017\r\nplain' >"$TMP/input"
roundtrip attributes 0 2 2

printf '\033[53moverline\033[mplain' >"$TMP/input"
roundtrip overline 0 1 1

printf 'x' >"$TMP/input"
source=$(pane "$TMP/input")
wait_dead "$source"
$TMUX capture-pane -p -r -t "$source" -S 0 -E 0 >"$TMP/replay"
printf 'x\033[K\r\n' >"$TMP/expected"
cmp "$TMP/expected" "$TMP/replay"
echo 'PASS exact short row'
$TMUX capture-pane -p -rJNT -t "$source" -S 0 -E 0 >"$TMP/replay"
cmp "$TMP/expected" "$TMP/replay"
for flag in L I F H C P R K; do
  if $TMUX capture-pane -p -r -"$flag" -t "$source" >"$TMP/error" 2>&1; then
    echo "FAIL accepted -r -$flag"; exit 1
  fi
done
echo 'PASS incompatible flags and ignored flags'

printf '\033[44m1234567890\033[m\r\n\033[43m\033[10X\033[m\r\nend' >"$TMP/input"
roundtrip full-and-erased 0 2 2

printf '\033[41m123456789界Z\033[m' >"$TMP/input"
roundtrip coloured-early-wrap 0 1 1

printf '%b' \
  '00\r\n01\r\n02\r\n03\r\n04\r\n05\r\n06\r\n07\r\n08\r\n09\r\n' \
  '10\r\n11\r\n12\r\n13' >"$TMP/input"
roundtrip history -2 2 4
concatenate hard 0 0 2
concatenate history-screen -2 -1 2

printf 'abcdefghijk' >"$TMP/input"
source=$(pane "$TMP/input")
wait_dead "$source"
concatenate soft 0 0 1

printf '\033[12;1Hbottom' >"$TMP/input"
source=$(pane "$TMP/input")
wait_dead "$source"
$TMUX capture-pane -p -r -t "$source" -S 11 -E 11 >"$TMP/replay"
printf 'bottom\033[K' >"$TMP/expected"
cmp "$TMP/expected" "$TMP/replay"
echo 'PASS grid last row has no separator'

printf '%b' \
  '\033[41m00\033[m\r\n01\r\n02\r\n03\r\n04\r\n05\r\n06\r\n07\r' \
  '\n08\r\n09\r\n10\r\n11\r\n12\r\n13' >"$TMP/input"
source=$(pane "$TMP/input")
wait_dead "$source"
$TMUX capture-pane -p -r -t "$source" -S -2 -E 11 >"$TMP/replay"
target=$(pane "$TMP/replay")
wait_dead "$target"
name=scroll-background; start=-2; end=11; rows=11; target_start=-2
compare
target_start=0

printf '\033[?1049hALTERNATE\033[42m\033[K\033[m' >"$TMP/input"
source=$(pane "$TMP/input")
wait_dead "$source"
$TMUX capture-pane -p -a -r -t "$source" -S 0 -E 1 >"$TMP/replay"
target=$(pane "$TMP/replay")
wait_dead "$target"
for flags in '-e -N' '-J' '' '-F'; do
  $TMUX capture-pane -p -a $flags -t "$source" -S 0 -E 1 >"$TMP/source"
  $TMUX capture-pane -p $flags -t "$target" -S 0 -E 1 >"$TMP/target"
  cmp "$TMP/source" "$TMP/target"
done
echo 'PASS alternate screen'

printf 'abcdefghijk\033[2;1H\033[X' >"$TMP/input"
source=$(pane "$TMP/input")
wait_dead "$source"
$TMUX capture-pane -p -r -t "$source" -S 0 -E 1 >"$TMP/replay"
printf 'abcdefghij\r\n\033[K\r\n' >"$TMP/expected"
cmp "$TMP/expected" "$TMP/replay"
echo 'PASS erased continuation uses hard separator'

printf 'abcdefghijk\033[1;6H\033[K' >"$TMP/input"
source=$(pane "$TMP/input")
wait_dead "$source"
$TMUX capture-pane -p -F -t "$source" -S 0 -E 0 >"$TMP/flags"
grep -q '^W ' "$TMP/flags"
$TMUX capture-pane -p -r -t "$source" -S 0 -E 1 >"$TMP/replay"
printf 'abcde\033[K\r\nk\033[K\r\n' >"$TMP/expected"
cmp "$TMP/expected" "$TMP/replay"
echo 'PASS shortened wrap uses hard separator'

printf '%b' \
  'text\033[3G\033_Ga=T,f=24,s=1,v=1,i=42,c=1,r=1,C=1,q=2;AAAA' \
  '\033\\\033[10G\033_Ga=p,i=42,p=2,c=1,r=1,C=1,q=2\033\\\033[6' \
  'Gend' >"$TMP/input"
source=$(pane "$TMP/input")
wait_dead "$source"
$TMUX capture-pane -p -rG -t "$source" -S 0 -E 1 >"$TMP/replay"
grep -Fq "$(printf '\033[3G\033_Ga=p,i=42,')" "$TMP/replay"
grep -Fq "$(printf '\033[10G\033_Ga=p,i=42,')" "$TMP/replay"
$TMUX capture-pane -p -K -t "$source" -S 0 -E 1 >"$TMP/images"
tr -d '\n' <"$TMP/images" >"$TMP/image-replay"
cat "$TMP/replay" >>"$TMP/image-replay"
target=$(pane "$TMP/image-replay")
wait_dead "$target"
name=images; start=0; end=1; rows=1
compare
for pane_id in "$source" "$target"; do
  $TMUX capture-pane -p -G -t "$pane_id" -S 0 -E 1 >"$TMP/$pane_id"
done
cmp "$TMP/$source" "$TMP/$target"
echo 'PASS image placement replay'
for flags in r rG; do
  $TMUX capture-pane -p -"$flags" -t "$source" \
    -S 0 -E 0 | tail -c 2 >"$TMP/separator"
  printf '\r\n' >"$TMP/expected"
  cmp "$TMP/separator" "$TMP/expected"
done
echo 'PASS hard separator is independent of placements'

printf '%b' \
  '\033_Ga=T,f=24,s=1,v=1,i=42,c=1,r=3,C=1,q=2;AAAA\033\\text\r' \
  '\nnext\r\nlast' >"$TMP/input"
source=$(pane "$TMP/input")
wait_dead "$source"
concatenate spanning-image 0 0 3 rG

printf '%b' \
  '\033_Ga=T,f=24,s=1,v=1,i=42,c=1,r=3,C=1,q=2;AAAA\033\\00\r\n' \
  '01\r\n02\r\n03\r\n04\r\n05\r\n06\r\n07\r\n08\r\n09\r\n10\r\n' \
  '11\r\n12\r\n13' >"$TMP/input"
source=$(pane "$TMP/input")
wait_dead "$source"
concatenate history-image -2 -1 2 rG

printf '%b' \
  'abcdefghijk\033[1G\033_Ga=T,f=24,s=1,v=1,i=42,c=1,r=1,C=1,q=' \
  '2;AAAA\033\\' >"$TMP/input"
source=$(pane "$TMP/input")
wait_dead "$source"
concatenate continuation-image 0 0 2 rG

printf '%b' \
  'abcdefghijk\033[1;4H\033_Ga=T,f=24,s=1,v=1,i=42,c=1,r=1,C=1,' \
  'q=2;AAAA\033\\' >"$TMP/input"
source=$(pane "$TMP/input")
wait_dead "$source"
for flags in r rG; do
  $TMUX capture-pane -p -"$flags" -t "$source" \
    -S 0 -E 0 | tail -c 3 >"$TMP/separator"
  printf 'hij' >"$TMP/expected"
  cmp "$TMP/separator" "$TMP/expected"
done
echo 'PASS soft separator is independent of placements'

image_position() {
  name=$1; row=$2; column=$3
  command="stty -opost -echo; cat '$TMP/input';
    printf '\033]2;ready\007'; read line;
    printf '\033[H${overlay:-0123456789ABCDEFGHIJabcdefghij}'"
  source=$($TMUX new-window -d -P -F '#{pane_id}' -t "${session:-replay}" \
    "$command")
  wait_for_output ready $TMUX display-message -p -t "$source" '#{pane_title}'
  $TMUX capture-pane -p -rG -t "$source" -S 0 -E 2 >"$TMP/replay"
  $TMUX capture-pane -p -K -t "$source" -S 0 -E 2 >"$TMP/images"
  tr -d '\n' <"$TMP/images" >"$TMP/image-replay"
  cat "$TMP/replay" >>"$TMP/image-replay"
  command="stty -opost -echo; cat '$TMP/image-replay';
    printf '\033]2;ready\007'; read line;
    printf '\033[H${overlay:-0123456789ABCDEFGHIJabcdefghij}'"
  target=$($TMUX new-window -d -P -F '#{pane_id}' -t "${session:-replay}" \
    "$command")
  wait_for_output ready $TMUX display-message -p -t "$target" '#{pane_title}'
  for pane_id in "$source" "$target"; do
    $TMUX capture-pane -p -t "$pane_id" -S 0 -E 2 | expand >"$TMP/$pane_id"
  done
  cmp "$TMP/$source" "$TMP/$target"
  if [ "$name" != tab ]; then
    for pane_id in "$source" "$target"; do
      $TMUX capture-pane -p -eN -t "$pane_id" -S 0 -E 2 >"$TMP/$pane_id"
    done
    cmp "$TMP/$source" "$TMP/$target"
  fi
  for pane_id in "$source" "$target"; do
    $TMUX send-keys -t "$pane_id" Enter
    wait_dead "$pane_id"
    $TMUX capture-pane -p -G -t "$pane_id" -S 0 -E 2 >"$TMP/$pane_id"
    awk -v name="$name" -v row="$row" -v column="$column" '
      index($0, "\033_Ga=p,i=42,") {
        if (NR != row + 1 || index($0, "\033_Ga=p,i=42,") != column + 1) {
          print "FAIL " name " image: row=" NR - 1 \
            ", column=" index($0, "\033_Ga=p,i=42,") - 1
          exit 1
        }
        found = 1
      }
      END {
        if (!found) {
          print "FAIL " name " image missing or misplaced"
          exit 1
        }
      }
    ' "$TMP/$pane_id"
  done
  cmp "$TMP/$source" "$TMP/$target"
  echo "PASS $name image position"
}
printf '%b' \
  '界abc\033[2G\033_Ga=T,f=24,s=1,v=1,i=42,c=1,r=1,C=1,q=2;AAAA' \
  '\033\\' >"$TMP/input"
image_position wide 0 1
printf '%b' \
  'a\tb\033[4G\033_Ga=T,f=24,s=1,v=1,i=42,c=1,r=1,C=1,q=2;AAAA' \
  '\033\\' >"$TMP/input"
image_position tab 0 3
printf '%b' \
  'abcdefghijk\033[1G\033_Ga=T,f=24,s=1,v=1,i=42,c=1,r=1,C=1,q=' \
  '2;AAAA\033\\' >"$TMP/input"
image_position continuation 1 0
printf '%b' \
  '\033]8;;https://example.com\033\\abcdefghijklmnop\033]8;;' \
  '\033\\\033[2;1H\033_Ga=T,f=24,s=1,v=1,i=42,c=1,r=1,C=1,q=2;A' \
  'AAA\033\\' >"$TMP/input"
image_position continuation-link 1 0
printf '%b' \
  'abcdefghijk\033[1;4H\033_Ga=T,f=24,s=1,v=1,i=42,c=1,r=1,C=1,' \
  'q=2;AAAA\033\\' >"$TMP/input"
image_position wrapped-row 0 3

printf '%b' \
  'abcdefghi界Z\033[2;1H\033[41m        \033[2;1H\t\033_Ga=T,f=2' \
  '4,s=1,v=1,i=42,c=1,r=1,C=1,q=2;AAAA\033\\' >"$TMP/input"
source=$(pane "$TMP/input")
wait_dead "$source"
for flags in r rG; do
  $TMUX capture-pane -p -"$flags" -t "$source" -S 0 -E 2 >"$TMP/replay"
  target=$(pane "$TMP/replay")
  wait_dead "$target"
  for pane_id in "$source" "$target"; do
    $TMUX capture-pane -p -eN -t "$pane_id" -S 0 -E 2 |
      awk '{ gsub(/\t/, "        "); print }' >"$TMP/$pane_id"
  done
  cmp "$TMP/$source" "$TMP/$target"
done
echo 'PASS coloured tab continuation uses hard separator'

printf 'abcdefghij\033[?1049h' >"$TMP/input"
source=$(pane "$TMP/input")
wait_dead "$source"
$TMUX resize-window -t "$source" -x 6
printf 'ghij\033[K\r\n' >"$TMP/expected"
for flags in ar arG; do
  $TMUX capture-pane -p -"$flags" -t "$source" -S 0 -E 0 >"$TMP/replay"
  cmp "$TMP/expected" "$TMP/replay"
done
echo 'PASS alternate capture uses current grid width'

printf 'hello\033_Ga=t,f=24,s=2,v=1,i=42,m=1;AAAA\033\\' >"$TMP/input"
source=$(pane "$TMP/input")
wait_dead "$source"
concatenate pending-upload 0 0 2 rG
concatenate pending-upload-last-row 0 0 11 rG
$TMUX capture-pane -p -rG -t "$source" -S 11 -E 11 >"$TMP/replay"
[ "$(grep -o AAAA "$TMP/replay" | wc -l | tr -d ' ')" = 1 ]
echo 'PASS pending upload belongs to grid last row'

session=narrow1; overlay=012
$TMUX new-session -d -s "$session" -x 1 -y 12
printf '%b' \
  'abc\033[2;1H\033_Ga=T,f=24,s=1,v=1,i=42,c=1,r=1,C=1,q=2;AAAA' \
  '\033\\' >"$TMP/input"
image_position one-column 1 0
session=narrow2; overlay=012345
$TMUX new-session -d -s "$session" -x 2 -y 12
printf '%b' \
  'ab界cd\033[2;1H\033_Ga=T,f=24,s=1,v=1,i=42,c=1,r=1,C=1,q=2;AA' \
  'AA\033\\' >"$TMP/input"
image_position two-column 1 0
