# Native review notes: 0.7.6.7

Nexus mod: https://www.nexusmods.com/baldursgate3/mods/24895

Published archive: `FirstPersonExplorer_0.7.6.7_Native_DX11_Vortex.zip`

Archive SHA-256:
`A3D864E324B60328D8BDB8C9C7F706FB7BB2F93821E8F9252665D3A4D0389F73`

Its sole file is `bin/NativeMods/FirstPersonExplorerNative.dll` (11264 bytes).
DLL SHA-256:
`69FB9C0B6AB349DA37B8E590342D57DA83FE16F10B2263780A12982DC142C3D8`

## Changes from 0.7.6.3

0.7.6.3 was flagged by some antivirus products (for example Microsoft Defender
`Trojan:Win32/Wacatac.H!ml`). About 150 KB of that DLL was statically linked MinGW
C-runtime startup code, with TLS callbacks, exported runtime symbols
(`_CRT_INIT`, `__mingw_module_is_dll`, `atexit`) and no version information.

0.7.6.7 is linked without a C runtime. It has no TLS callbacks, imports only these
KERNEL32 functions, and carries a version resource naming the product and this
repository:

`AcquireSRWLockExclusive, AcquireSRWLockShared, CloseHandle, CreateFileW, CreateThread,
DisableThreadLibraryCalls, GetModuleFileNameW, GetModuleHandleExW, GetModuleHandleW,
GetTickCount64, ReleaseSRWLockExclusive, ReleaseSRWLockShared, VirtualProtect, WriteFile`

Logging uses `CreateFileW`/`WriteFile` instead of `_wfopen`/`fprintf`. The hook,
registry and validation logic is unchanged and the existing native tests pass.

## What the DLL does

Native Mod Loader loads the DLL into the BG3 DX11 process. On startup it opens
`FirstPersonExplorerNative.log` beside itself for a startup/error message and checks
the executable timestamp, image size, four rendering vtables, and 32-byte method
fingerprints. Unsupported builds are rejected before hook installation.

It uses `VirtualProtect` and atomic pointer exchanges to replace three vtable slots
(scale, render and destructor) for those four known mesh implementations. It pins
its module so installed function pointers cannot refer to an unloaded DLL. Partial
installation failure triggers rollback of owned hooks.

The companion Lua code sends special scale-vector commands through Script Extender.
The native code consumes recognized probe/hide/show commands and keeps a bounded,
locked per-object registry with 500 ms leases. Hidden objects' render calls are
skipped; other render calls and ordinary scale operations are forwarded to the
original game functions. Destructor hooks remove registrations for destroyed objects.

The DLL makes no network requests, opens no other processes, and does not read
credentials, browser data or personal files. It does modify rendering-table pointers
inside the game process, as described above. The main Lua scripts communicate
through BG3's client/server mod API and retain startup/error/fallback reporting;
routine public-release camera and scale traces are disabled.

These notes describe the implementation; they do not assert why a scanner flagged
the file or substitute for a moderator's review. The exact source is included, and
BUILD.md describes how to compile and run tests.
