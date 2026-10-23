#!/bin/sh
# Synthetic classification test for the rescue precheck awk rules (PIT-157 W1/W2).
# Feeds fake "dir --detailed" output lines (formats taken from wimlib imagex.c)
# into the same awk rules used by zjrestore-lite.sh and checks the verdicts:
#   reparse: a tag is BAD when its hex digits (no 0x) are shorter than 8 chars,
#     or are exactly 8 chars and start with 0-7 (bit31 == 0 -> non-Microsoft).
#     Expected BAD: 0xdead, 0x1a, 0x78000000, 0x7fff0000, 0x00012345
#     Expected ok : 0x8000001a, 0xa0000003
#   EFS: only the anchored attribute line "    FILE_ATTRIBUTE_ENCRYPTED is set"
#     (4 leading spaces, imagex.c:2602-2615) counts; decoys without the exact
#     prefix must NOT count. Expected ENC=1 (one encrypted entry).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
IN="$HERE/test-reparse-awk.in"
OUT="$HERE/test-reparse-awk.out"
cat > "$IN" <<'EOF'
Reparse Tag         = 0x8000001a
Reparse Tag         = 0xa0000003
Reparse Tag         = 0xdead
Reparse Tag         = 0x1a
Reparse Tag         = 0x78000000
Reparse Tag         = 0x7fff0000
Reparse Tag         = 0x00012345
Attributes          = 0x00004020
    FILE_ATTRIBUTE_ARCHIVE is set
    FILE_ATTRIBUTE_ENCRYPTED is set
Attributes          = 0x00000020
    FILE_ATTRIBUTE_ARCHIVE is set
xxFILE_ATTRIBUTE_ENCRYPTED is set (decoy - must NOT count)
EOF
awk '/^Reparse Tag /{t=$NF; sub(/^0x/,"",t); if(length(t)<8 || (length(t)==8 && t ~ /^[0-7]/)) print "BADTAG 0x" t}
     /^    FILE_ATTRIBUTE_ENCRYPTED is set/ { enc++ }
     END { printf "ENC=%d\n", enc }' "$IN" > "$OUT"
EXPECT="BADTAG 0xdead
BADTAG 0x1a
BADTAG 0x78000000
BADTAG 0x7fff0000
BADTAG 0x00012345
ENC=1"
GOT="$(cat "$OUT")"
if [ "$GOT" = "$EXPECT" ]; then
    echo "AWK TEST PASS (2 ok survived, 5 bad flagged, enc=1)"
    rm -f "$IN" "$OUT"
    exit 0
fi
echo "AWK TEST FAIL"
echo "--- expected ---"; echo "$EXPECT"
echo "--- got ---";      echo "$GOT"
exit 1
