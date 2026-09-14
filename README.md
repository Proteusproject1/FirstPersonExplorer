# First Person Explorer

Source for First Person Explorer for
[Baldur's Gate 3](https://www.nexusmods.com/baldursgate3/mods/24895):
the **0.7.6.7 native DX11 DLL** and the 0.7.6.3 public-release main-mod Lua.

The mod hides the controlled character and supported equipment in first-person
exploration, with temporary scale and camera-height compensation for targeting.
It returns to normal visibility when leaving first person. Combat is excluded.

## Contents

- `public-release-0763/native/fpe_render.c`: complete native DX11 DLL source (0.7.6.7).
- `public-release-0763/native/fpe_version.rc`: DLL version information resource.
- `public-release-0763/native/verified_table.h`: exact-build table and method fingerprints.
- `public-release-0763/native/test_render.c`: native tests.
- `public-release-0763/src`: main-mod Lua, configuration, metadata and status definition from the 0.7.6.3 public release.
- [BUILD.md](BUILD.md): compiler, build and test instructions.
- [REVIEW.md](REVIEW.md): native behavior and released artifact checksums for review.

The folder keeps its `public-release-0763` name so existing links keep working.
SHA256SUMS.txt identifies the published source snapshot.

## 0.7.6.7 native changes

The DLL is rebuilt without a C runtime to make it smaller and easier to inspect:
193,536 bytes to 11,264 bytes, imports reduced to 14 KERNEL32 functions, no TLS
callbacks or runtime exports, and an embedded version resource naming the product and
this repository. Hook behavior is unchanged, and it is compatible with the current
Nexus main PAK. In-game load and body/equipment hiding were confirmed by the author.

## Runtime requirements

Windows x64; BG3 Steam DX11 build 4.1.1.7398727; Native Mod Loader;
Native Camera Tweaks; BG3 Script Extender 32+. Dependencies are installed separately.
Vulkan is not supported. Both the main PAK and native DLL are needed for
full equipment hiding. User-confirmed equipment fixes include violin, warhammer,
shortbow and morningstar; this is not a claim of exhaustive equipment coverage.

The author publishes this source to make the release inspectable. No third-party
game assets, NCT source, compiled toolchains, local logs or user data are included.
