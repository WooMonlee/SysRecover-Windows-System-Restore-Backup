#!/bin/bash
# Negative test for caps_selfcheck fail path (PIT-146).
# Run in git-bash:  bash tools/vmtest/test-caps-failpath.sh
# Extracts _capbin/_wim_ok/caps_selfcheck from bootfiles/zjrestore-lite.sh and
# runs them with stub environment where capabilities are absent -> must take the
# CAPS result=MISSING / STEP=caps-missing / persist_log / exit 1 branch and must
# NOT fall through past the check. Exit 0 = assertions passed.
# NOTE: never pipe `check` itself (pipeline elements run in subshells and the
# fail flag would be lost); compute grep rc first, then assert.
set -u
cd "$(dirname "$0")/../.." || exit 2

EXTRACT=$(mktemp) || exit 2
trap 'rm -f "$EXTRACT"' EXIT
awk '/^_capbin\(\)\{/{f=1} f{print} /^caps_selfcheck\(\)\{/{c=1} c && /^\}$/{exit}' \
    bootfiles/zjrestore-lite.sh > "$EXTRACT"
if ! grep -q 'caps_selfcheck(){' "$EXTRACT"; then
    echo "EXTRACT FAILED (function block not found)"
    exit 2
fi

# run_caps <engine> <undo_mark> <target_dev> -> prints everything, returns caps rc
run_caps() (
    set -u
    say(){ echo "SAY $*"; }
    persist_log(){ echo "STUB persist_log called"; }
    list_parts(){ :; }        # no block devices visible on host
    ENGINE="$1"
    UNDO_MARK="$2"
    TARGET_DEV="$3"
    TARGET_OFFSET=""
    ESP_INDEX=""
    EA_INDEX=""
    BB_HOME=""
    SOFT_MNT=""
    RESULT=UNKNOWN
    STEP=boot
    . "$EXTRACT"
    caps_selfcheck
    echo "REACHED-AFTER-CAPS"   # must never print
    exit 9
)

fail=0
check(){ # <label> <0-if-true-exit-code-or-cond...>  (no pipes allowed in args)
    local label="$1"
    shift
    if "$@"; then echo "  ok   $label"; else echo "  FAIL $label"; fail=1; fi
}
has(){   # <label> <pattern> <text>   -> grep match
    if echo "$3" | grep -q "$2"; then echo "  ok   $1"; else echo "  FAIL $1"; fail=1; fi
}
hasnt(){ # <label> <pattern> <text>  -> grep no-match
    if echo "$3" | grep -q "$2"; then echo "  FAIL $1"; fail=1; else echo "  ok   $1"; fi
}

echo "== case 1: block engine, bogus target =="
t1=$(run_caps block 0 /dev/no-such-zj-cap-dev 2>&1); r1=$?
echo "$t1" | sed 's/^/  | /'
check  "exit code == 1"              [ "$r1" -eq 1 ]
hasnt  "no fall-through"             "REACHED-AFTER-CAPS" "$t1"
has    "MISSING report present"      "CAPS result=MISSING:" "$t1"
has    "target-not-blockdev flagged" "target-not-blockdev" "$t1"
has    "mkntfs flagged (block)"      "mkntfs" "$t1"
has    "wimlib flagged (block)"      "wimlib-imagex" "$t1"
has    "persist_log called"          "STUB persist_log called" "$t1"
hasnt  "no ok verdict"               "CAPS result=ok" "$t1"

echo "== case 2: returnold, undo script absent =="
t2=$(run_caps swap 1 /dev/no-such-zj-cap-dev 2>&1); r2=$?
echo "$t2" | sed 's/^/  | /'
check  "exit code == 1"              [ "$r2" -eq 1 ]
has    "MISSING report present"      "CAPS result=MISSING:" "$t2"
has    "zjswap-undo.sh flagged"      "zjswap-undo.sh" "$t2"
hasnt  "no mkntfs demand for undo"   "mkntfs" "$t2"
has    "persist_log called"          "STUB persist_log called" "$t2"

if [ $fail -eq 0 ]; then
    echo "CAPS FAIL-PATH: PASS"
    exit 0
fi
echo "CAPS FAIL-PATH: FAIL"
exit 1
