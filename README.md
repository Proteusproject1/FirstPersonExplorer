# True First-Person Camera (formerly First Person Explorer)

Source for **True First-Person Camera 0.9.0** and its **Native DX11 0.7.7.0** component,
a first-person camera mod for [Baldur's Gate 3](https://www.nexusmods.com/baldursgate3/mods/24895)
covering exploration and optional combat.

The mod was renamed from First Person Explorer in 0.9.0. Only the displayed name changed:
the module UUID, folder (`FirstPersonExplorer`), DLL file name and log locations are the same,
so saves, load order and settings carry over.

## What's in 0.9.0

- **Settings in Mod Configuration Menu (MCM)**: first-person combat on/off, body style
  (Full size or Shrunken), and show held items. MCM is now a required dependency.
- **Full size** (new default): the body is hidden but not shrunk. Its physics group is made
  click-through so it never blocks targeting; drawn weapons, shields and a held torch stay
  visible; lingering buff visuals caused by the player or party are hidden while enemy and
  world effects stay visible.
- **Shrunken**: the earlier shrink-based behaviour, with footsteps played at a natural pace.
- **Native 0.7.7.0**: the engine code it hooks is found by masked signature instead of
  fixed addresses, so game hotfixes that only move code (like 2026-09-29) no longer break it.

## Contents

- `release-0.9.0/src`: main mod source (Lua, metadata, MCM blueprint, stats definitions).
- `release-0.9.0/localization/English.xml`: localization source (compiled to `.loca` by Divine).
- `release-0.9.0/native`: complete native DX11 DLL source and tests, plus
  [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause, `vendor/minhook/LICENSE.txt`).
- [BUILD.md](BUILD.md): compiler, build and test instructions.
- [REVIEW.md](REVIEW.md): what the native DLL does, and the released artifacts' hashes.
- `SHA256SUMS.txt`: hashes of this source snapshot.

Earlier releases remain available at the `v0.7.6.3` and `v0.7.6.7` tags.

## Runtime requirements

Windows x64, BG3 running in DirectX 11 (Vulkan unsupported), Native Mod Loader,
Native Camera Tweaks, BG3 Script Extender 32+, Mod Configuration Menu 1.40.1+.
Dependencies are installed separately; see the Nexus page for setup.

This repository contains source, not game assets or dependency binaries. It includes no
local logs, user data or compiled toolchains.
