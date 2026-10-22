#!/bin/zsh
# No system sleep at all: lid close and idle only turn the display off (XE-FBDPMS path, known to wake).
# This laptop is Modern Standby only (S3 hidden in the DSDT); a real sleep has never resumed on it.
# Run with sudo. The current settings are saved first (pmset-before-*.txt next to this script, under tools/build/).
# Then install the lid agent as your user: zsh tools/sleep/install-xe-lid.sh
# Undo: sudo zsh tools/sleep/90-pmset-revert.sh
set -e
[[ $EUID -eq 0 ]] || { echo "run: sudo zsh $0"; exit 1; }
OUT=${0:A:h:h}/build
mkdir -p $OUT
pmset -g custom > $OUT/pmset-before-$(date +%Y%m%d-%H%M%S).txt
pmset -a sleep 0            # no idle system sleep
pmset -a disablesleep 1     # lid close / Apple menu > Sleep no longer enter system sleep
pmset -a hibernatemode 0    # never write/resume a sleepimage (OpenCore HibernateMode=None)
pmset -a standby 0 powernap 0 proximitywake 0
pmset -c womp 0
pmset -b displaysleep 3     # display off after 3 min on battery
pmset -c displaysleep 10    # 10 min on AC
pmset -a halfdim 1
pmset -a disksleep 10
rm -f /var/vm/sleepimage
pmset -g
echo "Now, as your user (not sudo): zsh ${0:A:h}/install-xe-lid.sh"
