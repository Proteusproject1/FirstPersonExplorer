# First Person Explorer

Source for the **0.7.6.3 public release** of First Person Explorer for
[Baldur's Gate 3](https://www.nexusmods.com/baldursgate3/mods/24895).

The mod hides the controlled character and supported equipment in first-person
exploration, with temporary scale and camera-height compensation for targeting.
It returns to normal visibility when leaving first person. Combat is excluded.

## Contents

- `public-release-0763/native/fpe_render.c`: complete native DX11 DLL source.
- `public-release-0763/native/verified_table.h`: exact-build table and method fingerprints.
- `public-release-0763/native/test_render.c`: native tests.
- `public-release-0763/src`: exact main-mod Lua, configuration, metadata and status definition.
- [BUILD.md](BUILD.md): compiler, build and test instructions.
- [REVIEW.md](REVIEW.md): native behavior and released artifact checksums for review.

This repository contains source, not game assets or dependency binaries.
The release source files were copied byte-for-byte from the public-release build.
The standalone build wrappers make compilation possible without the original
development workspace. SHA256SUMS.txt identifies the published source snapshot.

## Runtime requirements

Windows x64; BG3 Steam DX11 build 4.1.1.7398727; Native Mod Loader;
Native Camera Tweaks; BG3 Script Extender 32+. Dependencies are installed separately.
Vulkan is not supported. Both main and native 0.7.6.3 components are needed for
full equipment hiding. User-confirmed equipment fixes include violin, warhammer,
shortbow and morningstar; this is not a claim of exhaustive equipment coverage.

The author publishes this source to make the release inspectable. No third-party
game assets, NCT source, compiled toolchains, local logs or user data are included.
