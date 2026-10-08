# True First-Person Camera (formerly First Person Explorer)

Source for **True First-Person Camera 3.0 (The Full Body Update)** and its **Native DX11 3.0.0.0**
component, a first-person camera mod for [Baldur's Gate 3](https://www.nexusmods.com/baldursgate3/mods/24895)
covering exploration and combat.

The mod was renamed from First Person Explorer in 0.9.0. Only the displayed name changed:
the module UUID, folder (`FirstPersonExplorer`), DLL file name and log locations are the same,
so saves, load order and settings carry over. Version 3.0 follows 0.9.0 (shown as "2" on Nexus).

## What's in 3.0

- **Full body first person** (new default): look down and see your own arms, armour and weapons.
  Only the head, hair and headwear are hidden. Head parts are recognised by their mesh names,
  with a shape-based fallback for modded heads.
- **Head camera** (Native 3.0): the eye follows the live head mesh (16 cm below its crown), so
  the camera moves with the body when running, stopping, leaning or swinging and fits every body
  size. In combat, or when the camera pivot is far from the head, the eye follows the head exactly.
- **First-person combat on by default**, with an **In combat** choice: Hands and weapons
  (default), Weapons only, Full body or Nothing. The camera stays in the head.
- The body hides itself while you walk toward the camera (more than 120 degrees from it).
- **Simpler settings**: one View choice (Full body / Hidden body / Shrunken body), an optional
  Fine-tune camera (eye height, eye forward, look-down tilt, head bob), and a Reset to
  recommended settings button. Diagnostic logs are off unless switched on.
- Without Native 3.0, or if a game update changes the camera code, Full body switches itself off
  and the mod falls back to the Hidden body view (the 0.9.0 behaviour).

## Contents

- `release-3.0.0/src`: main mod source (Lua, metadata, MCM blueprint, stats definitions).
- `release-3.0.0/localization/English.xml`: localization source (compiled to `.loca` by Divine).
- `release-3.0.0/native`: complete native DX11 DLL source and tests, plus
  [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause, `vendor/minhook/LICENSE.txt`).
- [BUILD.md](BUILD.md): compiler, build and test instructions.
- [REVIEW.md](REVIEW.md): what the native DLL does, and the released artifacts' hashes.
- `SHA256SUMS.txt`: hashes of this source snapshot.

Earlier releases remain available at the `v0.7.6.3`, `v0.7.6.7` and `v0.9.0` tags.

## Runtime requirements

Windows x64, BG3 running in DirectX 11 (Vulkan unsupported), Native Mod Loader,
Native Camera Tweaks (exploration and combat settings), BG3 Script Extender 32+,
Mod Configuration Menu 1.40.1+. Dependencies are installed separately; see the Nexus page for setup.

This repository contains source, not game assets or dependency binaries. It includes no
local logs, user data or compiled toolchains.
