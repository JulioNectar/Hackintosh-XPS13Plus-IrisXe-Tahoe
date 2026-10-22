# Licensing notice

- **This repository** (the NootedGreen fork in `src/`, the scripts, `macos/`, `tools/` except the libfprint patch noted
  below, and the documentation) is distributed under the **Thou Shalt Not Profit License 1.0** (see [LICENSE](LICENSE)),
  the license of the project it derives from, [NootedGreen](https://github.com/sgiammori/NootedGreen) by StezzaPilot.
  Free to use, study, modify and share; not for profit.
- **`src/HookCase/`** is HookCase by Steven Michaud, as modified in the NootedGreen project, with two fixes from this
  repository (probePortMode NULL `callback`, strnstr signature). It is under the **MIT License**; see
  [src/LICENSE-HookCase](src/LICENSE-HookCase).
- **Third-party components in `EFI/`** keep their own licenses: OpenCore, Lilu, WhateverGreen, VirtualSMC, NVMeFix,
  RestrictEvents, BrightnessKeys, CpuTopologyRebuild (acidanthera and contributors); itlwm, IntelBluetoothFirmware,
  IntelBTPatcher (OpenIntelWireless); VoodooI2C and VoodooPS2 (their authors); USBToolBox (dhinakg). See each
  project for its terms.
- **`EFI/OC/Kexts/AirportItlwm.kext`** is the unmodified release build of **AirportItlwm-Tahoe v2.4.0-alpha** from
  laobamac's fork of itlwm (commit `738d4c6`, executable sha256 `259bac94…973c`), licensed under the **GNU General
  Public License v2.0** like itlwm itself; the licence text is in [licenses/itlwm-GPL-2.0.txt](licenses/itlwm-GPL-2.0.txt).
  Corresponding source: <https://github.com/laobamac/itlwm> at that commit (release `v2.4.0-alpha`). It contains Intel
  Wi-Fi firmware under Intel's firmware licence (see the source repository).
- **Intel SOF audio firmware and topology are not included.** The speaker driver (`tools/AudShim`) is built against
  files you download from the official sof-bin release; `tools/AudShim/AudShimData.h`, which would contain the firmware,
  is generated locally and excluded from the repository. The firmware is distributed by Intel under its own licence
  (`LICENCE.Intel` in sof-bin).
- **libfprint, glib, libgusb and libusb are not included.** `tools/fingerprint/build.sh` downloads them from upstream
  (libfprint commit `6f9479c3`, glib 2.84.4, libgusb 0.3.10, libusb 1.0.29) and builds them locally. All four are
  under the **GNU Lesser General Public License v2.1 or later** ([licenses/LGPL-2.1.txt](licenses/LGPL-2.1.txt)).
  `tools/fingerprint/xefp-libfprint-macos.patch` modifies libfprint and is distributed under the same LGPL-2.1-or-later
  terms; the other files in `tools/fingerprint/` (xe-fp, pam_xefp) are this repository's own code and only link
  dynamically against those libraries.
- **Apple software is not included.** The scripts patch your own copy of Apple's GPUDriversIntel 16.0.32 bundles at
  install time. Apple, macOS, Metal and related names are trademarks of Apple Inc.; this project is not affiliated
  with or endorsed by Apple, Intel or Dell.
