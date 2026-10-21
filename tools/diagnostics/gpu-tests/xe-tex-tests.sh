#!/bin/zsh
# Roda os testes de xe-tex-tests do mais seguro ao mais arriscado, cada um num processo separado, com 10 s de limite.
# Para no primeiro que não terminar (= GPU travada: o WindowServer deve reiniciar em seguida).
# Resultado em ~/xe/logs/tex-tests-<data>.txt, gravado (sync) a cada teste para sobreviver a um travamento.
#   zsh ~/xe/tools/gpu-tests/xe-tex-tests.sh          todos menos wu_ios (o caso que já travou a GPU)
#   zsh ~/xe/tools/gpu-tests/xe-tex-tests.sh --tudo   inclui wu_ios
B=${0:A:h}/xe-tex-tests
OUT=~/xe/logs/tex-tests-$(date +%Y%m%d-%H%M%S).txt
TESTES=(bufu r8 uv_rg8 rgba8 uv_rgba8 amostra wf_plano wu4_tex wu_tex wu_plano)
[[ ${1:-} == --tudo ]] && TESTES+=(wu_ios)
typeset -A DESC=(bufu "controle: inteiro num buffer" r8 "ler plano Y como R8" uv_rg8 "ler plano UV como RG8"
  rgba8 "ler plano Y como RGBA8 (4 px/texel, como o CoreVideo)" uv_rgba8 "ler plano UV como RGBA8" amostra "amostrar plano Y como RGBA8 (sampler)"
  wf_plano "escrever float no plano Y como RGBA8" wu4_tex "escrever inteiros RGBA8Uint, textura comum"
  wu_tex "escrever inteiro R32Uint, textura comum" wu_plano "escrever inteiro R32Uint no plano Y" wu_ios "escrever R32Uint em IOSurface BGRA (travou em 20/10)")
echo "== $(date '+%F %T') $(sysctl -n kern.osversion)" > $OUT
for t in $TESTES; do
  printf "%-9s %-58s " $t "${DESC[$t]}" | tee -a $OUT; sync
  $B $t > $OUT.tmp 2>&1 &
  p=$!; n=0
  while kill -0 $p 2>/dev/null && (( n < 100 )); do sleep 0.1; (( n++ )); done
  if kill -0 $p 2>/dev/null; then
    echo "TRAVOU (>10 s) — parando a série" | tee -a $OUT; sync; exit 2
  fi
  cat $OUT.tmp | tr -d '\n' | tee -a $OUT; echo | tee -a $OUT; sync
done
rm -f $OUT.tmp; echo "== fim" >> $OUT; echo "resultado: $OUT"
