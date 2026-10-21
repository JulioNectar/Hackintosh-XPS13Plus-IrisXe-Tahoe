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
| Resolução nativa (3840×2400) e escala HiDPI | ✅ (troque com `tools/xe-res`, não pelos Ajustes) |
| Frequência da GPU | ✅ ~1300 MHz sob carga (pede 1500 MHz; o limite de energia do processador, PL4/IccMax, segura em ~1300), repouso em RC6 |
| Imagens HEIC/YUV (papéis de parede, fotos) | ✅ |
| Ícones de apps, Finder, miniaturas | ✅ |
| YouTube | ✅ no Chrome (com GPU, até 4K) · ✅ no Safari com uma saída de áudio (Bluetooth ou o BlackHole virtual, `tools/build-blackhole.sh`) |
| Gravação de tela | ✅ |
| Decodificação de vídeo por hardware | ✅ H.264 na GPU (VDBOX) para apps que usam o VideoToolbox, com `-ngreengva` e o plugin VA corrigido (`macos/vadriver/`) · ⚠️ o Safari ainda decodifica na CPU (sandbox do processo GPU do WebKit) · 🚧 HEVC: plugin corrigido, falta o XE-GVAHEVC no NootedGreen (em teste) |
| Teclas de brilho | ⚠️ o firmware da Dell parou de avisar as teclas de sol; F6/F7 funcionam pelo `tools/xe-brilho daemon` |
| Ajustes › Telas | ⚠️ pode dar pânico |
| Desligar / reiniciar | ✅ rápido (XE-IDLEBOUND; antes travava ~3 min e dava panic) |
| Travamento do WindowServer | ⚠️ às vezes antes do login e em algumas trocas de escala da tela (reiniciar resolve); correção em teste (csbresync) |

Som interno (SoundWire) e câmera **não funcionam** no macOS neste notebook.

## Documentação

- [Guia de instalação](docs/INSTALL.md)
- [Patches do kernel](docs/KERNEL-PATCHES.md): cada correção, o problema que resolve e o boot-arg que a desliga
- [Metal, libxedrv e compilador de shaders](docs/METAL.md), incluindo a correção dos ícones sumidos
- [Status e problemas conhecidos](docs/STATUS.md)
- [Como depuramos](docs/DEBUGGING.md): as ferramentas de teste que acharam cada bug

A documentação técnica está em inglês. Os comentários do código estão, em boa parte, em português.

## Créditos e licença

Base: **NootedGreen** de StezzaPilot (licença *Thou Shalt Not Profit 1.0*, que vale também para este fork). Referências: WhateverXe e o fork do OCLP de b00t0x, IntelIGPU-Tahoe, acidanthera, OpenIntelWireless, VoodooI2C. Desenvolvido num XPS 13 Plus pelo próprio dono, em parceria com o Claude (Anthropic).

**Nenhum binário da Apple está incluído.** Você precisa da sua própria cópia dos bundles 16.0.32.
