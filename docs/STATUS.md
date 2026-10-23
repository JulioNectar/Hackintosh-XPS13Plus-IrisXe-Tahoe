# Status (October 2026, macOS Tahoe 26.7.1 25G241)

## Works

- Metal desktop: WindowServer, Dock, Finder, Liquid Glass materials, Mission Control, animations at 60 Hz.
- 3840×2400 panel, HiDPI 1280–1920 pt (via `tools/xe-res`).
- GPU at ~1300 MHz under load (XE-GTFREQ requests 1500 MHz = RP0; `GT_PERF_LIMIT_REASONS` shows the PL4/IccMax package limit holding it at ~1300), RC6 when idle (XE-RC6FW, since 2026-10-22: before, a forcewake leak could keep the GPU awake at ~5 W).
- HEIC/YUV images, wallpapers (PLANES2). App icons, thumbnails and image layers (libxedrv v15).
- Video: YouTube in **Chrome with GPU acceleration** (software decode on the CPU), including 4K. Screen recording works. Safari works once an audio output exists: the internal speakers (XeSpeakers), Bluetooth, or the BlackHole virtual device (`tools/build-blackhole.sh`).
- Brightness through `xe-brilho` (XeShim).
- All 16 CPU threads (4 P-cores with HT + 8 E-cores).
- Fast shutdown/restart (XE-IDLEBOUND), trackpad and touchscreen on interrupts, multitouch gestures.
- **Internal speakers** (since 2026-10-22): `AudShim` (kext) runs Intel's SOF firmware on the audio DSP, SoundWire and
  the two RT1316 amplifiers; the `XeSpeakers` HAL plug-in makes them a normal output device (volume, mute, system
  sounds, music). Capped at −3 dB (AudShim 0.9.1), no speaker protection: see below.
- **Internal microphones** (since 2026-10-22): AudShim 0.9.0 also runs the RT714 microphone codec on SoundWire link 0
  and the SOF capture pipeline; XeSpeakers 0.9.0 adds the input device "Microfone interno" (48 kHz stereo).
- **Native Wi-Fi** (AirportItlwm-Tahoe, macOS Wi-Fi menu; no HeliPort). Bluetooth as before.
- Clean boot screen (sharp logo, no garbage flash) and lid close → display off (`xe-lid`).
- Touchpad click-vibration strength (`xe-haptic`, `xe-hapticbar` menu-bar slider).
- **Fingerprint for sudo** (since 2026-10-22): the Goodix 27C6:63BC sensor works through libfprint's goodixmoc driver
  (match-on-chip, built for macOS with a small patch) and a PAM module in `/etc/pam.d/sudo_local` (`tools/fingerprint`).
  Login, screen unlock, Touch ID settings, Apple Pay and LocalAuthentication prompts are **not** supported: they need
  the Secure Enclave, which a hackintosh does not have.
- **Power button locks the screen**: a short press locks at once, like a Mac with Touch ID (`tools/xe-pblock`, user
  LaunchAgent). Touching the fingerprint sensor in the button does not trigger it.

## Known problems

| Problem | Details | Workaround |
|---|---|---|
| WindowServer hang | fixed 2026-10-21 (csbresync): not seen again in daily use since | — |
| GPU hang → reboot | any hang on the render or compute engine (e.g. Photos' `mediaanalysisd`) used to end in a restart loop and a WindowServer watchdog panic: the reset of the shared render domain left the other engine unconfigured. Fixed 2026-10-22 (XE-GPUREC): three real hangs on 2026-10-23 (two natural, one induced by an encode experiment) were reset without a restart, **but the screen still froze**: the render-domain reset (GDRST bit 1) leaves the compute engine (CCS) disabled, because RCU_MODE (0x14800) bit 0 is only programmed by `IntelAccelerator::initModeRegisters` at boot/wake and is in neither of Apple's reset-register lists. WindowServer's CCS channels (stamps 24–31) then never complete ("no channel associated with stamp_idx", watchdog). XE-GPUREC v2 re-enables the CCS right after the reset; installed, waiting for a real hang (`-ngreennogpurecex` turns it off) | `launchctl disable gui/$UID/com.apple.mediaanalysisd` removes the most common trigger |
| Slow shutdown / watchdog panic | fixed 2026-10-21 (XE-IDLEBOUND): shutdown no longer waits on a GPU that never goes idle | — |
| Panic at shutdown (rare) | "corrupt list" in `IOAccelOrphanedMemoryPool` (release path) | none yet |
| System Settings › Displays and colour profiles | fixed in practice: no panics since 21 Oct, including colour-profile changes (re-tested 22 Oct). The three page-fault panics of 20 Oct (ITU-709) stopped after XE-RCPOLICY, csbresync and libxedrv v16/v17; the exact root cause was never isolated | — |
| Brightness keys | the sun keys work on some boots and not on others: when they fail the EC still raises events but the Dell SMM does not report brightness (no `Notify(LCD)`), so BrightnessKeys gets nothing. Firmware state (e.g. not after entering the BIOS); not graphics-related (OpRegion/ASLE checked) | `xe-brilho daemon` with `~/.xe-brilho-teclas` binding F6/F7 (`down 97 800000` / `up 98 800000`) |
| Trackpad / touchscreen | fixed 2026-10-20: GPIO/APIC interrupts instead of forced polling (the I2C bus used to get stuck, "Timeout waiting for bus not to be busy") | — |
| Hardware video decode | **H.264 works** (2026-10-21) with `-ngreengva` + the patched VA plugin (`macos/vadriver/build-vadriver.sh`, installed in `/Library/GPUBundles`): frames are bit-exact against software decode, for VideoToolbox clients outside the WebKit GPU-process sandbox (verified with `xe-vtdec` and in-process decoder tests). **Safari still decodes on the CPU**: its `com.apple.WebKit.GPU` sandbox denies `file-map-executable` for `/Library/GPUBundles`, so the plugin cannot load there. **HEVC**: the plugin fixes are in the same build (bit-exact in tests, I/P/B frames); the accelerator property `IOGVAHEVCDecode` comes from NootedGreen XE-GVAHEVC; **confirmed on the GPU 2026-10-21** (30/30 frames, no hang). Hardware encode is untested | software decode (Safari) |
| E-cores | fixed 2026-10-21: all 16 threads run (see below), grouped by CpuTopologyRebuild | — |
| Internal speakers | **work** (2026-10-22): AudShim 0.9.1 + XeSpeakers. Limits: 48 kHz stereo only; the kext caps the DSP gain at **−3 dB** (−6 dB before 0.9.1) (`audshimcapdb=0..20` changes it) because the topology has **no speaker protection** (no Smart Amp/temperature model), so very loud playback for long periods is at your own risk; the DSP and SoundWire links stay powered while the device is open (power cost not measured); no headphone/HDMI routing (the laptop has no jack). The firmware boots when coreaudiod loads the plug-in (~1–2 s) | `uninstall-audio.sh hal`; emergency `-audshimoff` |
| Microphones | **work** (2026-10-22): "Microfone interno", 48 kHz stereo. The input level slider is a software gain (0 to +30 dB, default +12 dB); no echo cancellation or noise suppression beyond what apps do themselves; the DSP stays powered while the input is open | — |
| System sleep | **none**: the firmware is Modern Standby (S0ix) only, `_S3` is hidden in the DSDT, and the TGL framebuffer cannot retrain the eDP link after a power cycle. Recommended setup: `tools/sleep/10-pmset-safe.sh` (no sleep, display sleep only) + `xe-lid` (lid → display off). The machine stays awake with the lid closed; shut down for long breaks | display sleep |
| Lid / sun-key events | on some boots the EC does not report lid or brightness-key events (same firmware state as the brightness keys below); then `xe-lid` does not see the lid | turn the display off by hand (`pmset displaysleepnow`, a hot corner); F6/F7 for brightness |
| Wi-Fi | AirportItlwm-Tahoe 2.4.0-alpha: WPA2 and WPA2/WPA3 transition networks; WPA3-only networks, mandatory PMF and AWDL (AirDrop, AirPlay to/from the Mac, Sidecar) are not supported by the driver; Wi-Fi 6 rates are off unless `itlwm_he=1`. After a macOS update that changes IO80211Family it may stop loading: boot `config-install.plist` (itlwm) and wait for a new build | itlwm + HeliPort |
| Camera | OmniVision OV01A10 sensor behind an Intel Visual Sensing Controller (VSC) on the IPU6; no macOS driver, work not started | USB camera / Continuity Camera |
| Extended range 10-bit (`w30r` / BGR10_XR) | same as on real Intel Macs: Metal only accepts the XR formats on Apple-family GPUs, CoreAnimation only maps `w30r` when the device is Apple2+, and Gen12 has no XR-bias surface format. A proof of concept that flips the CoreAnimation bit also switches 10-bit video to Apple-only formats and would crash WindowServer, so it is not shipped | — |
| Lock-screen clock | fixed 2026-10-21 (libxedrv v17): the 16.0.32 driver aligned Stencil8 render targets to 4 KB instead of 64 KB; a stencil sharing a 64 KB block with a ≥32 bpp colour target in the same pass lost every odd 64-byte line, giving streaks in the Liquid Glass clock | — |
| Chrome UI broken when it starts with a GPU disk cache | toolbar icons missing, `border-radius` ignored, Google logo drawn as a bar. **Cause found (21/10):** not the cache itself but a race in the 16.0.32 Metal driver's blits: a `fillBuffer`/aligned copy (2D blit path) is not ordered before later 3D work in the same command buffer, so Dawn's zero-fill overwrote Graphite's static index buffer after the upload (it stayed all zero; per-launch race, almost always lost when the cache makes startup faster). **Fixed by libxedrv v16**, which routes every buffer fill/copy through the 3D path ([METAL.md](METAL.md) §3 item 6); test Chrome profile: 12/12 launches correct with v16 vs 2/12 without | — |
| Boot screen | fixed 2026-10-21: OpenCore Resolution=Max (native GOP, no 1024x768 stretch stage) + WhateverGreen `gfxrst=1` (skips the ~18 s copy of the boot screen) + NootedGreen XE-BOOTHOLD (backlight and plane off until WindowServer's buffer ring has been drawn once). The logo is sharp from the start, no line/garbage flashes, first frame at ~20 s | — |
| WKWebView apps: dark box over tall cards | Apple's CPU fallback for IOSurface-backed CoreGraphics contexts mis-renders `box-shadow` on elements taller than ~8,000 device px (the shadow is not clipped and darkens the element). WebKit uses that fallback because its GPU/WebContent processes cannot load our Metal driver from /Library/GPUBundles (sandbox). Not a driver bug: with the driver loaded the same drawing is correct | per app: `defaults write <bundle-id> WebKitDebugAcceleratedDrawingEnabled -bool NO` (or `-g` for all non-sandboxed WKWebView apps; Safari not covered) |
| Login-screen wallpaper black with an Aerial | **fixed 2026-10-23 (libxedrv v18)**: wide IOSurface textures (≥32768 bytes per row, e.g. the 5632 px half-float frame macOS renders from an Aerial) were never copied by the kernel, so the snapshot came out black. See [METAL.md](METAL.md) fix 8. A snapshot made before the fix stays black until the wallpaper is set again | — |
| Images green or black (Apple Music artwork, some photos in WhatsApp) | **fixed 2026-10-23 (XE-BLITFIX)**: IOSurface copies with a row pitch not a multiple of 64 bytes and larger than 128 KB went to the kernel's Blit3D path, which does nothing on this GPU, so the GPU read zeros (YUV → green, RGB → black). They now use the copy engine ([#6](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe/issues/6)) | `-ngreennoblitfix` turns it off |
| Transition glitch | a brief wrong frame during UI-scale changes (`xe-res`). The login → desktop glitch is gone since libxedrv v16 | cosmetic |

## E-cores

The i7-1360P is a 4P+8E part. With any E-core enabled the kernel froze right after `EXITBS`, before any verbose text,
even with a single E-core. Cause: on a hybrid CPU OpenCore's `ProvideCurrentCpuInfo` patches the kernel's
cores-per-package value to 128, assuming CPUID leaf 1 reports `logical_per_package = 128` (true for 6P+8E and desktop
parts). The 4P+8E Raptor Lake-P reports **64**, so xnu computes threads-per-core = 64/128 = 0 and divides by it in
`cpu_thread_alloc` (`initTopoParms`). Fix: the existing `Cpuid1Data` spoof also sets EBX[23:16] = 0x80
(`Cpuid1Data 55060A00 00008000 00000000 00000000`, `Cpuid1Mask FFFFFFFF 0000FF00 00000000 00000000`). Result:
`hw.ncpu = 16`. CpuTopologyRebuild 2.0.2 is enabled in its default mode (no HT kernel patch needed): the E-cores
become extra SMT threads of the P-cores (`hw.physicalcpu = 4`, `hw.logicalcpu = 16`), which the author recommends for
performance; System Information shows the real 12 cores. `ctrsmt=off` would expose the real topology but is slower.

How each of these was reached, and what was already tried, is in [HISTORY.md](HISTORY.md) and
[EXPERIMENTS.md](EXPERIMENTS.md).

## Not possible on this laptop (so far)

- System sleep (S3/S0ix resume), see above. Hibernation and an injected `_S3` were prepared but not tried
  ([EXPERIMENTS.md](EXPERIMENTS.md) #80).
- AirDrop, AWDL and Continuity features that need Apple Wi-Fi.
- An on-demand touchpad pulse (Force Touch feedback): the haptic module only fires on physical clicks
  ([EXPERIMENTS.md](EXPERIMENTS.md) #82).
- Camera (OV01A10 + Intel VSC + IPU6): no driver work started.
- Safari hardware video decode: needs the VA plugin in `/System/Library/Extensions` (a sealed-volume root patch,
  prepared but not applied; [EXPERIMENTS.md](EXPERIMENTS.md) #81).

## Reports from other machines

Not verified here: I only have the XPS 13 Plus 9320.

- **Dell Latitude 7430** (i7-1265U, Iris Xe 46A8, 96 EU, 1080p panel, macOS 26.7.1), reported 22 Oct ([#2](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe/issues/2)):
  - Works: Metal, hardware decode (`-ngreengva` + the VA plugin), AirportItlwm, touchpad and touchscreen. The XOSI/I2C
    ACPI gave the Synaptics touchpad its GPIO interrupt.
  - The missing `HookCase.kext` was found on this install: now in `src/HookCase` + `build-hookcase.sh`.
  - Known issues there:
    - GPU hangs (context 5, `WAIT_ON_SCANLINE` in the gpuRestart report) when opening System Settings › Displays,
      System Settings › Trackpad or Photo Booth: the render engine (RCS) hangs on the dummy page `0xc01dbead`. That
      machine has a plain UVC webcam, so the camera path runs. The reporter ruled out the 48 Hz mode and mode switching.
    - Occasional screen corruption when viewing screenshots.
- **i5-1334U / i5-1340P** (80 EU): rendering artifacts ([#3](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe/issues/3)).
- **ASUS VivoBook K3502ZA** (i5-12500H, Iris Xe 46A6, 80 EU): boot stall with the acceleration stack ([#4](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe/issues/4)).

The 80 EU reports match a known limit: NootedGreen hard-codes the 96 EU topology for every non-Tiger-Lake GPU (see
[INSTALL.md](INSTALL.md)). Issues and PRs from other machines are welcome.

## iServices

iMessage/FaceTime/iCloud readiness was checked read-only (22 Oct): SMBIOS fields set and consistent, en0 is the
built-in Wi-Fi port, APNs connects, no activation errors. `amfi_get_out_of_my_way=1` can still affect camera/mic
permission prompts. Generate your own SMBIOS with `make-efi.sh` and never reuse someone else's.
