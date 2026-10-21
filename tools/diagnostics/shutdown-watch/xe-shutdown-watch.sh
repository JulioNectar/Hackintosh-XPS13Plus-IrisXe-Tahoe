#!/bin/zsh
# xe-shutdown-watch: diagnóstico do desligamento travado (LaunchDaemon local.xe.shutdownwatch, roda como root).
# Fica parado até o launchd mandar SIGTERM (início do desligamento). A partir daí grava, a cada 3 s, os processos
# que ainda não saíram em /Users/Shared/xe-shutdown/<data>.txt (com sync, para sobreviver a um panic), e um
# spindump (pilhas de todos os threads, inclusive do kernel) aos 20 s e aos 90 s.
# Sai sozinho quando só restam o kernel, o launchd, ele mesmo, zumbis e drivers DriverKit (.dext, que só saem junto com o
# kernel), para não atrasar o desligamento. Antes de sair grava um spindump (onde o kernel e os .dext estão parados).
# 20/10: a 1ª versão contava o próprio ps/awk como "restantes", nunca saía e atrasava o desligamento ~60 s.
OUT_DIR=/Users/Shared/xe-shutdown; mkdir -p $OUT_DIR
SELF=$$
desligando=0
trap 'desligando=1' TERM
while (( ! desligando )); do sleep 1; done

OUT=$OUT_DIR/$(date +%Y%m%d-%H%M%S).txt
T0=$SECONDS
echo "== desligamento começou $(date '+%F %T')" >> $OUT
while (( SECONDS - T0 < 175 )); do
  t=$(( SECONDS - T0 ))
  todos=$(ps -axo pid=,ppid=,stat=,etime=,comm= | awk -v s=$SELF '$1!=0 && $1!=1 && $1!=s && $2!=s && $5!="ps" && $5!="awk"')
  restantes=$(print -r -- "$todos" | grep -vE ' Z+ |<defunct>|/DriverExtensions/' | grep -v '^$')
  { echo "-- t=${t}s $(date +%T)"; echo "$todos"; } >> $OUT
  if (( t >= 20 && t < 23 )) || (( t >= 90 && t < 93 )); then
    spindump -notarget 2 10 -stdout >> $OUT 2>&1
  fi
  sync
  # SIGTERM sem desligamento (launchctl bootout, reinstalação): com dezenas de processos vivos depois de 6 s, sai.
  if (( t >= 6 )) && (( $(print -r -- "$restantes" | grep -c .) > 40 )); then
    echo "== alarme falso (não é desligamento): saindo" >> $OUT; break
  fi
  if [[ -z $restantes ]]; then
    echo "== só restam zumbis/.dext; spindump final e saída" >> $OUT
    spindump -notarget 1 10 -stdout >> $OUT 2>&1; sync; break
  fi
  sleep 3
done
echo "== fim do registro $(date +%T)" >> $OUT; sync
