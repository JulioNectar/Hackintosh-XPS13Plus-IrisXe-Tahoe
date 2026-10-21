#!/bin/zsh
# xe-sleep-diag.sh: diagnóstico da tela que não acorda depois do repouso da tela.
#   zsh ~/xe/tools/xe-sleep-diag.sh    (segundo plano; saída em ~/xe/logs/sleep-*.txt)
# t=0 dump | t=5 pmset displaysleepnow | t=15 dump | t=20 acorda (caffeinate -u) | t=30 dump
# t=35 tentativa 1: PP_CONTROL power+backlight, BLC_CTL enable, duty salvo | t=45 tentativa 2: plano + SURF | t=55 final
set -u
X=~/xe/build/xectl
OUT=~/xe/logs/sleep-$(date +%Y%m%d-%H%M%S).txt
REGS=(PIPECONF_A 0x70008 PIPESRC_A 0x6001C PLANE_CTL_1A 0x70180 PLANE_SURF_1A 0x7019C PLANE_SURFLIVE_1A 0x701AC
      PLANE_SIZE_1A 0x70190 PS_CTRL_1A 0x68180 CUR_CTL_A 0x70080 DDI_BUF_CTL_A 0x64000 DP_TP_CTL_A 0x64040
      TRANS_DDI_FUNC_CTL_A 0x60400 PP_STATUS 0xC7200 PP_CONTROL 0xC7204 BLC_CTL 0xC8250 BLC_FREQ 0xC8254
      BLC_DUTY 0xC8258 PWR_WELL_CTL2 0x45404 DC_STATE_EN 0x45504 CHICKEN_MISC_2 0x42084 DBUF_CTL_S1 0x44FE8)
dump() { echo "== $1 ($(date +%T))" >> $OUT; for ((i = 1; i < ${#REGS[@]}; i += 2)); do printf "%-22s %s\n" ${REGS[i]} "$($X rd ${REGS[i+1]} 2>&1)" >> $OUT; done; }
v() { $X rd $1 | awk '{print $3}'; }
(
  dump "antes"; DUTY=$(v 0xC8258)
  sleep 5; echo "== pmset displaysleepnow ($(date +%T))" >> $OUT; pmset displaysleepnow >> $OUT 2>&1
  sleep 10; dump "dormindo"
  sleep 5; echo "== acorda: caffeinate -u ($(date +%T))" >> $OUT; caffeinate -u -t 3 >> $OUT 2>&1 &
  sleep 10; dump "depois de acordar"
  sleep 5; echo "== tentativa 1: PP_CONTROL/BLC ($(date +%T))" >> $OUT
  p=$(v 0xC7204); $X wr 0xC7204 $(printf '0x%x' $(( 0x$p | 0x5 ))) >> $OUT 2>&1
  b=$(v 0xC8250); $X wr 0xC8250 $(printf '0x%x' $(( 0x$b | 0x80000000 ))) >> $OUT 2>&1
  $X wr 0xC8258 0x$DUTY >> $OUT 2>&1
  sleep 2; dump "após tentativa 1"
  sleep 8; echo "== tentativa 2: plano ($(date +%T))" >> $OUT
  c=$(v 0x70180); (( (0x$c & 0x80000000) == 0 )) && $X wr 0x70180 $(printf '0x%x' $(( 0x$c | 0x80000000 ))) >> $OUT 2>&1
  s=$(v 0x7019C); $X wr 0x7019C 0x$s >> $OUT 2>&1
  sleep 8; dump "final"; echo "== fim ($(date +%T))" >> $OUT
) &
echo "diagnóstico rodando; saída: $OUT"
