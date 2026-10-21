#!/bin/zsh
# sudo zsh tools/diagnostics/shutdown-watch/instalar.sh            instala e inicia
# sudo zsh tools/diagnostics/shutdown-watch/instalar.sh desfazer   remove
set -e
D=${0:A:h}; P=/Library/LaunchDaemons/local.xe.shutdownwatch.plist; S=/usr/local/libexec/xe-shutdown-watch.sh
launchctl bootout system/local.xe.shutdownwatch 2>/dev/null || true
pkill -9 -f xe-shutdown-watch.sh 2>/dev/null || true   # a versão antiga trata o SIGTERM do bootout como desligamento
for i in {1..20}; do launchctl print system/local.xe.shutdownwatch >/dev/null 2>&1 || break; sleep 0.5; done
if [[ ${1:-} == desfazer ]]; then rm -f $P $S; echo "removido"; exit 0; fi
mkdir -p /usr/local/libexec
install -o root -g wheel -m 755 $D/xe-shutdown-watch.sh $S
install -o root -g wheel -m 644 $D/local.xe.shutdownwatch.plist $P
launchctl bootstrap system $P
launchctl print system/local.xe.shutdownwatch | grep -E "state|pid"
