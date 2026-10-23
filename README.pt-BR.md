# Aceleração da Iris Xe no macOS Tahoe — Dell XPS 13 Plus (9320)

Aceleração gráfica de verdade (**Metal, Core Animation, Liquid Glass**) para a **Intel Iris Xe do i7-1360P (Raptor Lake-P, 8086:A7A0)** no **macOS Tahoe 26.7.1**, num hackintosh com OpenCore.

A Apple nunca fez driver para o Raptor Lake. Este projeto roda **o driver que a própria Apple fez para a Iris Xe do Tiger Lake** (GPUDriversIntel 16.0.32, de Macs que nunca foram lançados) na GPU mais nova, porque as duas têm a mesma arquitetura (Xe-LP, Gen12). Um **NootedGreen modificado** (plugin do Lilu) corrige as diferenças no kernel, e um conjunto de patches de user space adapta esse driver de 2020 ao Metal do Tahoe.

> 🚧 **Projeto em andamento: este repositório é um retrato vivo do trabalho.** O desenvolvimento acontece dia a dia
> no próprio XPS 13 Plus do autor, e cada correção entra aqui depois de confirmada no hardware real. Atualizações
> frequentes; veja o [CHANGELOG.md](CHANGELOG.md) (o que mudou e o que ainda está em teste).

## Já tem instalado? Atualize

Se o projeto já roda no seu notebook, para atualizar (as novidades estão no [CHANGELOG.md](CHANGELOG.md)):

1. **Faça backup da EFI** de onde você liga (copie a pasta `EFI` ou tenha uma segunda EFI num pendrive).
2. No seu clone do repositório, num disco APFS:
   ```sh
   git pull
   zsh install.sh          # recompila e reinstala o NootedGreen, o driver Metal, o libxedrv e o compilador de shaders
   ```
3. Copie o novo `build/NootedGreen.kext` por cima de `EFI/OC/Kexts/NootedGreen.kext` na EFI de onde você liga.
4. Reinicie e confira: `kmutil showloaded | grep NootedGreen`.

Não rode o `make-efi.sh` de novo para atualizar: ele gera **novos** números de série (SMBIOS), e isso desconecta o
iCloud/iMessage. Mantenha a sua EFI e troque só o kext.

Se não ligar, use a EFI de backup. Cada correção pode ser desligada com o boot-arg indicado no CHANGELOG.
Detalhes: [docs/INSTALL.md](docs/INSTALL.md#updating-an-existing-install).

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
| YouTube | ✅ Chrome com GPU, até 4K (VP9/AV1 decodificados na CPU) · ✅ Safari até 1080p H.264 (precisa de uma saída de áudio: os alto-falantes internos, Bluetooth ou o BlackHole virtual) |
| Gravação de tela | ✅ |
| Tela de toque | ✅ até 10 dedos pelo VoodooI2C; o `tools/xe-toque` acrescenta rolagem com um dedo, segurar = clique direito, gestos pela borda, caneta, lupa e modo toque |
| Trackpad e teclado | ✅ trackpad e tela de toque por interrupção (sem os travamentos do polling) |
| Brilho | ✅ `tools/xe-brilho` (o slider do macOS agora cobre toda a faixa do painel com `ngreenblmax=17777`) · ⚠️ as teclas de sol dependem de um estado do firmware da Dell que muda de um boot para outro; F6/F7 sempre funcionam pelo `xe-brilho daemon` |
| Desligar / reiniciar | ✅ rápido (XE-IDLEBOUND; antes travava ~3 min e dava panic) |
| Tela de boot | ✅ logo da Apple nítido desde o início, sem lixo na tela (OpenCore `Resolution=Max` + `gfxrst=1` + XE-BOOTHOLD) |
| Alto-falantes internos | ✅ o nosso kext `AudShim` roda o firmware SOF da Intel no DSP de áudio, o SoundWire e os dois amplificadores RT1316; o `XeSpeakers` vira uma saída de som normal do macOS (volume, mudo). Limitado a −3 dB, sem proteção dos alto-falantes · ✅ microfones internos (RT714) como **"Microfone interno"** |
| Wi-Fi, Bluetooth | ✅ Wi-Fi nativo (menu do macOS) com o AirportItlwm-Tahoe, sem HeliPort · ❌ AirDrop/AWDL · ✅ Bluetooth (IntelBluetoothFirmware) |
| Vibração do trackpad | ✅ intensidade do clique com `tools/xe-haptic` / o controle `xe-hapticbar` na barra de menus · ❌ sem pulso sob demanda (resposta do Force Touch) |
| Leitor de digitais | ✅ para o `sudo` (Goodix 27C6:63BC via libfprint, módulo PAM no `sudo_local`; `tools/fingerprint`) · ❌ login, desbloqueio da tela e Touch ID (sem Secure Enclave) |
| Botão liga/desliga | ✅ um aperto curto bloqueia a tela (`tools/xe-pblock`) |
| Repouso | ❌ sem repouso do sistema (firmware só tem Modern Standby); ✅ repouso da tela, e fechar a tampa apaga a tela (`tools/sleep`) |

A câmera (OV01A10 atrás do IPU6 da Intel) **não funciona** no macOS; o leitor de digitais funciona só para o `sudo`.
**Os alto-falantes internos funcionam** desde 22/10/2026, sem nenhum driver de áudio da Apple: o `AudShim` liga o DSP de áudio da Intel (cAVS 2.5)
com o firmware SOF da Intel (baixado do sof-bin oficial na instalação, não vem neste repositório), inicia o SoundWire e
os amplificadores RT1316, e o plug-in `XeSpeakers` publica a saída de som e o microfone ([docs/INSTALL.md](docs/INSTALL.md#internal-speakers-and-microphones)).

**Outros notebooks** (Raptor Lake / Alder Lake): por enquanto o NootedGreen fixa a topologia da GPU de 96 EU
(12 subslices × 8 EU) para toda GPU que não é Tiger Lake, então os chips de 80 EU (i5-1334U, i5-1340P, i5-12500H)
recebem uma topologia errada ([#3](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe/issues/3),
[#4](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe/issues/4)). Veja o [guia de instalação](docs/INSTALL.md).

## Documentação

- [Guia de instalação](docs/INSTALL.md)
- [Patches do kernel](docs/KERNEL-PATCHES.md): cada correção, o problema que resolve e o boot-arg que a desliga
- [Metal, libxedrv e compilador de shaders](docs/METAL.md), incluindo a correção dos ícones sumidos
- [Status e problemas conhecidos](docs/STATUS.md)
- [Como depuramos](docs/DEBUGGING.md): as ferramentas de teste que acharam cada bug
- [Histórico](docs/HISTORY.md) fase a fase e [experimentos que falharam](docs/EXPERIMENTS.md)

A documentação técnica está em inglês. Os comentários do código estão, em boa parte, em português.

## Créditos e licença

Base: **NootedGreen** de StezzaPilot (licença *Thou Shalt Not Profit 1.0*, que vale também para este fork). Referências: WhateverXe e o fork do OCLP de b00t0x, o trabalho do jkbuha no WhateverXe, IntelIGPU-Tahoe, acidanthera, OpenIntelWireless, laobamac (AirportItlwm-Tahoe, GPL-2.0, incluído na EFI), Sound Open Firmware e os drivers SOF/SoundWire do Linux, BlackHole, VoodooI2C. Desenvolvido num XPS 13 Plus pelo próprio dono, em parceria com o Claude (Anthropic).

**Nenhum binário da Apple está incluído.** Você precisa da sua própria cópia dos bundles 16.0.32.
