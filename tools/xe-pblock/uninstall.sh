#!/bin/sh
# Removes the local.xe.pblock LaunchAgent and the installed app (the log stays).
#   sh tools/xe-pblock/uninstall.sh [--reset-tcc]   --reset-tcc also removes the Input Monitoring grant
LBL=local.xe.pblock
launchctl bootout gui/$(id -u)/$LBL 2>/dev/null && echo "agent stopped" || echo "agent was not loaded"
rm -f "$HOME/Library/LaunchAgents/$LBL.plist" && echo "plist removed"
rm -rf "$HOME/Library/Application Support/xe/xe-pblock.app"
[ "$1" = "--reset-tcc" ] && tccutil reset ListenEvent $LBL
exit 0
