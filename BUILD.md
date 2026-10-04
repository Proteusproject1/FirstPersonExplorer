# Build 0.9.0 / Native 0.7.7.0

## Native DX11 component

Use Windows x64, PowerShell and **Zig 0.16.0 for Windows x86_64** from the
[official Zig download page](https://ziglang.org/download/#release-0.16.0)
(`zig-x86_64-windows-0.16.0.zip`). No Visual Studio installation or running game is needed.

Open PowerShell in the repository root, replacing the compiler path with yours:

```powershell
./build-native.ps1 -Zig 'C:\Tools\zig-x86_64-windows-0.16.0\zig.exe'
```

The script:

1. builds and runs `test_render` (render bridge: dispatch, registry, expiry, threading,
   protected-slot install and rollback) and `test_scale_diag` (instant-scale mailbox and the
   real mid-function hook through MinHook, including CPU-state preservation);
2. if BG3 is installed at the default Steam path (or `-GameExe` points to `bg3_dx11.exe`),
   builds and runs `test_locate`: it maps the executable read-only the way the Windows loader
   does, checks that the hooked code is found, then that tampered copies are refused. Its
   expected addresses are for game build stamp 1789998461, so it fails on other builds;
3. compiles each unit, links the DLL without a C runtime using Zig's COFF linker, and
   writes `dist/FirstPersonExplorerNative.dll` and
   `dist/TrueFirstPersonCamera_Native_DX11_0.7.7.0_Vortex.zip`
   (`bin/NativeMods/FirstPersonExplorerNative.dll` plus `MinHook-LICENSE.txt`);
4. prints the DLL hash with the 4-byte PE build timestamp zeroed and compares it with the
   released DLL. The linker stamps the build time into the header, so that is the only
   difference a rebuild has from the released binary.

No obfuscator, packer, post-build binary patch or code-signing step is used.

## Main PAK

Use [LSLib/Divine](https://github.com/Norbyte/lslib/releases). There is no compilation step:

```powershell
Divine.exe --action convert-loca --source release-0.9.0/localization/English.xml --destination release-0.9.0/src/Localization/English/FirstPersonExplorer.loca --game bg3
Divine.exe --action create-package --source release-0.9.0/src --destination TrueFirstPersonCamera_0.9.0_Main.pak --game bg3
```

The PAK's Lua was exercised in development against mocked Script Extender APIs (203
scenarios) and tested in game; that harness depends on the development workspace and is
not part of this repository.
