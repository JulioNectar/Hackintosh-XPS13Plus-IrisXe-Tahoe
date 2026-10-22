# Aceleração da Iris Xe no macOS Tahoe — Dell XPS 13 Plus (9320)

Aceleração gráfica de verdade (**Metal, Core Animation, Liquid Glass**) para a **Intel Iris Xe do i7-1360P (Raptor Lake-P, 8086:A7A0)** no **macOS Tahoe 26.7.1**, num hackintosh com OpenCore.

A Apple nunca fez driver para o Raptor Lake. Este projeto roda **o driver que a própria Apple fez para a Iris Xe do Tiger Lake** (GPUDriversIntel 16.0.32, de Macs que nunca foram lançados) na GPU mais nova, porque as duas têm a mesma arquitetura (Xe-LP, Gen12). Um **NootedGreen modificado** (plugin do Lilu) corrige as diferenças no kernel, e um conjunto de patches de user space adapta esse driver de 2020 ao Metal do Tahoe.

> 🚧 **Projeto em andamento: este repositório é um retrato vivo do trabalho.** O desenvolvimento acontece dia a dia
> no próprio XPS 13 Plus do autor, e cada correção entra aqui depois de confirmada no hardware real. Atualizações
> frequentes; veja o [CHANGELOG.md](CHANGELOG.md) (o que mudou e o que ainda está em teste).

> ⚠️ **Experimental.** Ainda acontecem travamentos da GPU e kernel panics de vez em quando. Tenha sempre um segundo disco de boot (ou o instalador no USB). Leia [docs/STATUS.md](docs/STATUS.md) antes de começar.

## O que funciona

| Área | Status |
|---|---|
| Mesa, Dock, janelas, desfoque "Liquid Glass" | ✅ acelerado pela GPU (Metal) |
| Resolução nativa (3840×2400) e escala HiDPI | ✅ troque com `tools/xe-res`, não pelos Ajustes (um glitch rápido na troca é só visual) |
| Frequência da GPU | ✅ ~1300 MHz sob carga (pede 1500 MHz; o limite de energia do processador, PL4/IccMax, segura em ~1300), repouso em RC6 |
| Processador | ✅ **os 16 threads (4 núcleos P com HT + 8 núcleos E)**: os núcleos E travavam o boot; corrigido com um byte no `Cpuid1Data`. O CpuTopologyRebuild 2.0.2 (modo padrão) agrupa como 4 núcleos P com 4 threads cada, então o trabalho pesado vai para os núcleos P; Informações do Sistema mostra 12 núcleos |
| Imagens HEIC/YUV (papéis de parede, fotos) | ✅ |
| Ícones de apps, Finder, miniaturas | ✅ (o formato de 10 bits com faixa estendida `w30r`/`BGR10_XR` é exclusivo dos chips Apple: nem os Macs Intel de verdade o desenham) |
| Decodificação de vídeo por hardware | ✅ **H.264 na GPU** (idêntico ao software) para apps que usam o VideoToolbox: QuickTime, Fotos, editores, conversões. Precisa de `-ngreengva` e do plugin VA corrigido (`macos/vadriver/build-vadriver.sh`, instalação manual por enquanto) · ✅ **HEVC** também (XE-GVAHEVC no NootedGreen) · ⚠️ o Safari decodifica na CPU: o processo GPU do WebKit não pode carregar drivers de `/Library/GPUBundles` |
| YouTube | ✅ Chrome com GPU, até 4K (VP9/AV1 decodificados na CPU) · ✅ Safari até 1080p H.264 (precisa de uma saída de áudio: Bluetooth ou o BlackHole virtual, `tools/build-blackhole.sh`) |
| Gravação de tela | ✅ |
| Tela de toque | ✅ até 10 dedos pelo VoodooI2C; o `tools/xe-toque` acrescenta rolagem com um dedo, segurar = clique direito, gestos pela borda, caneta, lupa e modo toque |
| Trackpad e teclado | ✅ trackpad e tela de toque por interrupção (sem os travamentos do polling) |
| Brilho | ✅ `tools/xe-brilho` (o slider do macOS agora cobre toda a faixa do painel com `ngreenblmax=17777`) · ⚠️ as teclas de sol dependem de um estado do firmware da Dell que muda de um boot para outro; F6/F7 sempre funcionam pelo `xe-brilho daemon` |
| Desligar / reiniciar | ✅ rápido (XE-IDLEBOUND; antes travava ~3 min e dava panic) |
| Ajustes › Telas, perfis de cor | ⚠️ pode dar panic: troque a resolução com `tools/xe-res` e mantenha o perfil de cor de fábrica |
| Travamento do WindowServer | ⚠️ raro, antes do login ou numa troca de escala (reiniciar resolve); correção em teste (csbresync) |

A câmera **não funciona** no macOS. **Alto-falantes internos: em andamento** — o nosso kext `AudShim` liga o DSP de áudio da Intel (cAVS 2.5) com o firmware SOF, inicia o SoundWire, configura os dois amplificadores RT1316 e já tocou um tom de teste nos dois alto-falantes (22/10/2026); a saída de som do macOS (CoreAudio) vem a seguir.

## Documentação

- [Guia de instalação](docs/INSTALL.md)
- [Patches do kernel](docs/KERNEL-PATCHES.md): cada correção, o problema que resolve e o boot-arg que a desliga
- [Metal, libxedrv e compilador de shaders](docs/METAL.md), incluindo a correção dos ícones sumidos
- [Status e problemas conhecidos](docs/STATUS.md)
- [Como depuramos](docs/DEBUGGING.md): as ferramentas de teste que acharam cada bug

A documentação técnica está em inglês. Os comentários do código estão, em boa parte, em português.

## Créditos e licença

Base: **NootedGreen** de StezzaPilot (licença *Thou Shalt Not Profit 1.0*, que vale também para este fork). Referências: WhateverXe e o fork do OCLP de b00t0x, o trabalho do jkbuha no WhateverXe, IntelIGPU-Tahoe, acidanthera, OpenIntelWireless, VoodooI2C. Desenvolvido num XPS 13 Plus pelo próprio dono, em parceria com o Claude (Anthropic).

**Nenhum binário da Apple está incluído.** Você precisa da sua própria cópia dos bundles 16.0.32.
