#!/bin/zsh
# xe-modeset-diag.sh: diagnóstico da tela preta (cursor visível) depois de trocar o modo de tela.
#   zsh tools/diagnostics/xe-modeset-diag.sh w1024      (roda sozinho em segundo plano; saída em ~/xe/logs/modeset-*.txt)
# Linha do tempo (contada a partir do início):
#   t=0   dump "antes"            t=5   troca o modo (xe-res)        t=15  dump "depois"
#   t=20  tentativa 1: reescreve PLANE_SURF_1_A com o próprio valor (força o latch do flip)
#   t=30  tentativa 2: liga o bit de enable do PLANE_CTL_1_A (se estiver desligado) + reescreve SURF
#   t=40  tentativa 3: reaplica CHICKEN_MISC_2 bit30 (sem DPT) + DBUF S1 + GAMMA bypass, reescreve SURF
#   t=50  dump final
set -u
MODO=${1:-w1024}
# xectl / xe-res: tools/build/ (docs/INSTALL.md) or $PATH
TB=${0:A:h:h}/build
tool() { local p=$TB/$1; [[ -x $p ]] || p=$(command -v $1) || { echo "$1 not found: build it into tools/build/ (docs/INSTALL.md; xectl: clang -O2 -o tools/build/xectl tools/xectl.c -framework IOKit)" >&2; return 1; }; echo $p; }
X=$(tool xectl) || exit 1
XERES=$(tool xe-res) || exit 1
OUT=~/xe/logs/modeset-$(date +%Y%m%d-%H%M%S).txt
mkdir -p ~/xe/logs
REGS=(
  PIPECONF_A 0x70008 PIPESRC_A 0x6001C HTOTAL_A 0x60000 VTOTAL_A 0x6000C
  PLANE_CTL_1A 0x70180 PLANE_STRIDE_1A 0x70188 PLANE_POS_1A 0x7018C PLANE_SIZE_1A 0x70190
  PLANE_SURF_1A 0x7019C PLANE_OFFSET_1A 0x701A4 PLANE_SURFLIVE_1A 0x701AC PLANE_AUXDIST_1A 0x701C0
  PLANE_COLOR_CTL_1A 0x701CC PLANE_BUF_CFG_1A 0x7027C PLANE_WM0_1A 0x70240
  PLANE_CTL_2A 0x70280 PLANE_SURF_2A 0x7029C PLANE_SIZE_2A 0x70290
  CUR_CTL_A 0x70080 CUR_BASE_A 0x70084 CUR_POS_A 0x70088
  PS_CTRL_1A 0x68180 PS_WIN_POS_1A 0x68170 PS_WIN_SZ_1A 0x68174
  CHICKEN_MISC_2 0x42084 DBUF_CTL_S1 0x44FE8 DBUF_CTL_S2 0x44300 GAMMA_MODE_A 0x4A480
  TRANS_CONF_A 0x70008 PP_STATUS 0xC7200 PP_CONTROL 0xC7204 BLC_CTL 0xC8250 BLC_DUTY 0xC8258
)
dump() {
  echo "== $1 ($(date +%T))" >> $OUT
  for ((i = 1; i < ${#REGS[@]}; i += 2)); do
    printf "%-18s %s\n" ${REGS[i]} "$($X rd ${REGS[i+1]} 2>&1)" >> $OUT
  done
  local surf=$($X rd 0x7019C 2>/dev/null | awk '{print $3}')
  [[ -n $surf ]] && echo "GGTT PTE da SURF: $($X gtt $(( 0x$surf >> 12 )) 2>&1)" >> $OUT
}
reescreve_surf() { local v=$($X rd 0x7019C | awk '{print $3}'); $X wr 0x7019C 0x$v >> $OUT 2>&1; }
(
  dump "antes"
  sleep 5; echo "== troca de modo: $MODO ($(date +%T))" >> $OUT
  ( $XERES $MODO >> $OUT 2>&1 & )
  sleep 10; dump "depois da troca"
  sleep 5; echo "== tentativa 1: reescreve SURF ($(date +%T))" >> $OUT; reescreve_surf; sleep 2; dump "após tentativa 1"
  sleep 3; echo "== tentativa 2: enable do PLANE_CTL ($(date +%T))" >> $OUT
  c=$($X rd 0x70180 | awk '{print $3}'); (( (0x$c & 0x80000000) == 0 )) && $X wr 0x70180 $(printf '0x%x' $(( 0x$c | 0x80000000 ))) >> $OUT 2>&1
  reescreve_surf; sleep 2; dump "após tentativa 2"
  sleep 3; echo "== tentativa 3: CHICKEN/DBUF/GAMMA ($(date +%T))" >> $OUT
  m=$($X rd 0x42084 | awk '{print $3}'); $X wr 0x42084 $(printf '0x%x' $(( 0x$m | 0x40000000 ))) >> $OUT 2>&1
  d=$($X rd 0x44FE8 | awk '{print $3}'); $X wr 0x44FE8 $(printf '0x%x' $(( 0x$d | 0x80000000 ))) >> $OUT 2>&1
  $X wr 0x4A480 0x0 >> $OUT 2>&1
  reescreve_surf; sleep 8; dump "final"
  echo "== fim ($(date +%T))" >> $OUT
) &
echo "diagnóstico rodando em segundo plano; saída: $OUT"
