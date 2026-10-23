#!/bin/bash
# Behaviour test for the rescue failure menu (PIT-148 P3 + W4, 2026-10-19).
# Run in git-bash:  bash tools/vmtest/test-menu-loop.sh
#
# Extracts menu_loop() from bootfiles/alpine/init and runs it with stubs
# (the QEMU autorrecover drill only covers the STAGE=0 reboot path, where the
# menu never appears; this covers the menu itself):
#   * timeout / empty input        -> auto [2] reboot (prompt says auto-[2])
#   * invalid input                -> re-prompt, must NOT drop to shell
#   * [1] retry (stub exits rc=1)  -> menu returns, NO 3-round cap (round 4+)
#   * RESULT/STEP/rc               -> reprinted each round from the log
#   * bare shell                   -> ONLY via explicit [3]
set -u
cd "$(dirname "$0")/../.." || exit 2

TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT

# Extract the function and make the hard-coded dependencies testable:
#   read timeout 120 -> ${ZJ_MENU_TMO:-120}; log path -> $ZJ_MENU_LOG;
#   lite script -> $ZJ_STUB; exec /bin/sh -> STUB-SHELL marker.
awk '/^menu_loop\(\)\{/{f=1} f{print} f && /^\}$/{exit}' bootfiles/alpine/init \
    | sed -e 's#read -t 120 -r _ch#read -t ${ZJ_MENU_TMO:-120} -r _ch#' \
          -e 's#/tmp/zjrestore.log#"$ZJ_MENU_LOG"#g' \
          -e 's#/bin/sh /zjrestore-lite.sh#/bin/sh "$ZJ_STUB"#g' \
          -e 's#exec /bin/sh#echo STUB-SHELL#g' > "$TMP/menu.sh"
if ! grep -q 'menu_loop(){' "$TMP/menu.sh"; then
    echo "EXTRACT FAILED (menu_loop not found)"
    exit 2
fi
if ! grep -q 'STUB-SHELL' "$TMP/menu.sh"; then
    echo "EXTRACT FAILED (exec replacement did not apply)"
    exit 2
fi

# stub "lite script": every retry fails
printf '#!/bin/sh\nexit 1\n' > "$TMP/lite.sh"

# run_menu <stdin-text> -> console output (runs in a subshell; reboot exits it)
run_menu() (
    say(){ echo "SAY $*"; }
    reboot_now(){ echo "STUB-REBOOT"; exit 0; }
    TARGET_DEV=/dev/zjtest
    ZJ_MENU_TMO=${ZJ_MENU_TMO:-120}
    ZJ_MENU_LOG="$TMP/menu.log"
    ZJ_STUB="$TMP/lite.sh"
    . "$TMP/menu.sh"
    menu_loop
)

fail=0
ok(){ echo "  ok   $1"; }
bad(){ echo "  FAIL $1"; fail=1; }
has(){    # <label> <text> <pattern>
    if printf '%s' "$2" | grep -q "$3"; then ok "$1"; else bad "$1"; fi
}
hasnt(){
    if printf '%s' "$2" | grep -q "$3"; then bad "$1"; else ok "$1"; fi
}

echo "== case 1: timeout (empty stdin) -> auto [2] reboot =="
t1=$(ZJ_MENU_TMO=1 run_menu < /dev/null 2>&1)
echo "$t1" | sed 's/^/  | /'
has   "menu shown"            "$t1" "SysRecover recovery menu (round 1)"
has   "auto reboot"           "$t1" "STUB-REBOOT"
hasnt "no round 2"            "$t1" "round 2"
hasnt "no bare shell"         "$t1" "STUB-SHELL"

echo "== case 2: invalid input -> re-prompt, shell only via [3] =="
t2=$(printf '9\n3\n' | run_menu 2>&1)
echo "$t2" | sed 's/^/  | /'
has "invalid echoed"          "$t2" "invalid choice: 9"
has "menu re-prompted"        "$t2" "SysRecover recovery menu (round 2)"
has "shell after [3]"         "$t2" "STUB-SHELL"
hasnt "no reboot on typo"     "$t2" "STUB-REBOOT"

echo "== case 3: retries beyond 3 rounds (no cap) =="
t3=$(printf '1\n1\n1\n1\n3\n' | run_menu 2>&1)
echo "$t3" | sed 's/^/  | /'
has "round 4 exists"          "$t3" "SysRecover recovery menu (round 4)"
has "round 5 exists"          "$t3" "SysRecover recovery menu (round 5)"
has "retry rc printed"        "$t3" "restore script returned (rc=1)"
has "shell after [3]"         "$t3" "STUB-SHELL"

echo "== case 4: RESULT/STEP/rc reprinted from log =="
cat > "$TMP/menu.log" <<'EOF'
2026-10-19 10:00:00 ZJ: restore script returned (rc=7)
2026-10-19 10:00:01 ZJ: recover decision: menu (retries exhausted, stage=3)
2026-10-19 10:00:02 ZJ: recover decision: reboot (target untouched, STEP=image-not-found)
EOF
t4=$(printf '2\n' | run_menu 2>&1)
echo "$t4" | sed 's/^/  | /'
has "reprint rc+step"         "$t4" "RESULT: restore FAILED (rc=7) STEP=image-not-found"
has "reboot on [2]"           "$t4" "STUB-REBOOT"

echo
if [ "$fail" -eq 0 ]; then
    echo "MENU LOOP TEST: PASS"
    exit 0
fi
echo "MENU LOOP TEST: FAIL"
exit 1
