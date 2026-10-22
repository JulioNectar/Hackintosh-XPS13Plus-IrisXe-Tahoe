#!/bin/zsh
# Restores the macOS laptop defaults that were in place before 10-pmset-safe.sh (sleep on, hibernatemode 3, standby).
# Note: with these settings closing the lid starts a real system sleep, which this laptop cannot resume from.
# Run with sudo. Remove the lid agent separately: zsh tools/sleep/install-xe-lid.sh --remove
set -e
[[ $EUID -eq 0 ]] || { echo "run: sudo zsh $0"; exit 1; }
pmset -a disablesleep 0
pmset -a sleep 1 hibernatemode 3 standby 1 standbydelaylow 10800 standbydelayhigh 86400 highstandbythreshold 50 displaysleep 0 disksleep 10 halfdim 1 tcpkeepalive 1 ttyskeepawake 1 lidwake 1 lowpowermode 0
pmset -c powernap 1 proximitywake 1 womp 1
pmset -b powernap 0 proximitywake 0 lessbright 1
pmset -g custom
