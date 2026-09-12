# Build 0.7.6.3

## Native DX11 component (the quarantined file)

Use Windows x64, PowerShell and **Zig 0.16.0 for Windows x86_64** from the
[official Zig download page](https://ziglang.org/download/#release-0.16.0).
The Windows compiler archive is named `zig-x86_64-windows-0.16.0.zip`.
No Visual Studio installation, game files or running game are needed to compile
the native DLL or run its tests. The tests exercise local fake render objects;
they do not attach to a game process.

Download this repository, extract it, and open PowerShell in its root directory.
Replace the compiler path below with the location where you extracted Zig:

```powershell
./build-native.ps1 -Zig 'C:\Tools\zig-x86_64-windows-0.16.0\zig.exe'
```

This builds and runs `test_render.exe`, builds `FirstPersonExplorerNative.dll`,
and packages it at `bin/NativeMods/FirstPersonExplorerNative.dll` inside a Vortex ZIP
under `dist`. There are no nested archives or additional binaries in that ZIP.

The compiler commands used for the public release, from the repository root, are:

```powershell
zig cc public-release-0763/native/test_render.c -O2 -Wall -Wextra -Werror -o public-release-0763/native/test_render.exe
./public-release-0763/native/test_render.exe
zig cc public-release-0763/native/fpe_render.c -shared -O2 -Wall -Wextra -Werror -o public-release-0763/native/FirstPersonExplorerNative.dll
```

No obfuscator, packer, post-build binary patch or code-signing step is used.
Build-generated PDB, LIB and test EXE files are not distributed in the mod ZIP.
Use the released hashes in REVIEW.md to identify the exact submitted files.
Source hashes are in SHA256SUMS.txt. A rebuild's ZIP hash can differ because ZIPs
include file timestamps; bit-identical output is not a requirement of this script.

## Main PAK (optional for native review)

Use [LSLib/Divine v1.20.4](https://github.com/Norbyte/lslib/releases/tag/v1.20.4).
The main package has no compilation step: Divine packages seven source files.
Set the paths below to your checkout and extracted Divine executable:

```powershell
$repo = (Get-Location).Path
$divine = 'C:\Tools\ExportTool\Packed\Tools\Divine.exe'
New-Item -ItemType Directory -Force -Path (Join-Path $repo 'dist') | Out-Null
$pak = Join-Path $repo 'dist/FirstPersonExplorer_0.7.6.3_Public_Release_Main.pak'
& $divine --action create-package --source (Join-Path $repo 'public-release-0763/src') --destination $pak --game bg3
if ($LASTEXITCODE -ne 0) { throw 'PAK creation failed' }
Compress-Archive -LiteralPath $pak -DestinationPath (Join-Path $repo 'dist/FirstPersonExplorer_0.7.6.3_Public_Release_Main_Vortex.zip') -Force
```

The ZIP contains a single PAK at its root. The main mod depends on the native DLL
at runtime, but its packaging does not depend on compiling that DLL.
