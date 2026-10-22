#!/bin/sh
# Builds xe-pblock, checks that touching the fingerprint sensor does NOT produce a power event and that a press
# does, then installs the user LaunchAgent local.xe.pblock (no sudo).
#   sh tools/xe-pblock/install.sh [--skip-probe] [--fp-active]
#   --fp-active  run "xe-fp verify" during the touch test so the sensor is really capturing
# The app is copied to ~/Library/Application Support/xe/xe-pblock.app. Log: ~/Library/Logs/xe-pblock.log
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DEST="$HOME/Library/Application Support/xe"
APP="$DEST/xe-pblock.app"
BIN="$APP/Contents/MacOS/xe-pblock"
PL="$HOME/Library/LaunchAgents/local.xe.pblock.plist"
LOG="$HOME/Library/Logs/xe-pblock.log"
LBL=local.xe.pblock
XEFP=${XEFP:-/usr/local/libexec/xefp/xe-fp}
SKIP=0; FP=0
for a in "$@"; do case "$a" in --skip-probe) SKIP=1;; --fp-active) FP=1;; *) echo "unknown option: $a"; exit 2;; esac; done

sh "$HERE/build.sh"
mkdir -p "$DEST"
rm -rf "$APP"; cp -R "$HERE/../build/xe-pblock.app" "$APP"
"$BIN" --check-lock || { echo "SACLockScreenImmediate missing; aborting"; exit 1; }

if [ $SKIP = 0 ]; then
  echo
  echo "== Test 1/2: TOUCH the sensor (do not press) several times during the next 15 s =="
  FPPID=
  if [ $FP = 1 ] && [ -x "$XEFP" ]; then
    "$XEFP" verify --timeout 15 >/dev/null 2>&1 & FPPID=$!
    echo "(xe-fp verify running: the sensor is capturing)"
  fi
  printf "Enter to start... "; read _
  set -- $("$BIN" --count 15)
  [ -n "$FPPID" ] && kill $FPPID 2>/dev/null || true
  if [ "$1" != 0 ]; then
    echo "FAILED: touching produced $1 power event(s) (hid=$2 tap=$3). NOT installed."; exit 1
  fi
  echo "ok: touching produced no power event."
  echo
  echo "== Test 2/2: PRESS the power button ONCE (short press) during the next 15 s =="
  printf "Enter to start... "; read _
  set -- $("$BIN" --count 15)
  if [ "$1" = 0 ]; then
    echo "FAILED: no press seen (hid=$2 tap=$3). Give Terminal 'Input Monitoring' and run again. NOT installed."; exit 1
  fi
  echo "ok: press seen (hid=$2 tap=$3)."
fi

mkdir -p "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"
cat > "$PL" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>$LBL</string>
  <key>ProgramArguments</key><array><string>$BIN</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>30</integer>
  <key>ProcessType</key><string>Interactive</string>
  <key>LimitLoadToSessionType</key><string>Aqua</string>
  <key>StandardErrorPath</key><string>$LOG</string>
</dict></plist>
PL
plutil -lint "$PL" >/dev/null
launchctl bootout gui/$(id -u)/$LBL 2>/dev/null || true
launchctl bootstrap gui/$(id -u) "$PL"
sleep 2
echo; echo "== agent loaded; log =="; tail -n 5 "$LOG"
if tail -n 5 "$LOG" | grep -q "Input Monitoring: no"; then
  echo
  echo "Permission missing: System Settings > Privacy & Security > Input Monitoring > turn on 'xe-pblock'"
  echo "(if it is not listed, use + and choose $APP)."
  open "x-apple.systempreferences:com.apple.preference.security?Privacy_ListenEvent" 2>/dev/null || true
  printf "After turning it on, press Enter to restart the agent... "; read _
  launchctl kickstart -k gui/$(id -u)/$LBL; sleep 2; tail -n 3 "$LOG"
fi
echo
echo "Final test: press the button now; the screen should lock. Then check: tail $LOG"
