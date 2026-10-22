#!/bin/zsh
# Lid agent (no sudo): builds xe-lid and loads it as a user LaunchAgent. Closing the lid turns the display off
# (pmset displaysleepnow, the XE-FBDPMS path that wakes); opening it declares user activity. It never asks for system sleep.
# Use it together with 10-pmset-safe.sh (disablesleep 1), otherwise macOS tries a real sleep on lid close.
#   zsh tools/sleep/install-xe-lid.sh            build + install + load
#   zsh tools/sleep/install-xe-lid.sh --remove   unload + remove
# Log: /tmp/xe-lid.log
set -e
D=${0:A:h}
BIN=${XE_LID_BIN:-$HOME/Library/Application Support/xe/xe-lid}
PL=$HOME/Library/LaunchAgents/local.xe.lid.plist
if [[ "$1" == "--remove" ]]; then
    launchctl bootout gui/$(id -u)/local.xe.lid 2>/dev/null || true
    rm -f "$PL" "$BIN"
    echo "xe-lid removed"; exit 0
fi
mkdir -p "${BIN:h}" "${PL:h}"
clang -O2 -Wall -o "$BIN" $D/xe-lid.c -framework IOKit -framework CoreFoundation
sed "s#__XE_LID__#$BIN#" $D/local.xe.lid.plist > "$PL"
plutil -lint "$PL" >/dev/null
launchctl bootout gui/$(id -u)/local.xe.lid 2>/dev/null || true
launchctl bootstrap gui/$(id -u) "$PL"
echo "xe-lid loaded ($BIN). Test: close the lid for 10 s, open it; see /tmp/xe-lid.log"
