# Native review notes: 0.7.6.3

Nexus mod: https://www.nexusmods.com/baldursgate3/mods/24895

Submitted archive: `FirstPersonExplorer_0.7.6.3_Public_Release_Native_DX11_Vortex.zip`

Archive SHA-256:
`3B1DA1C3097079A50F87A78DF4324EC46A114FDCAF6475ECFF3FF0DEC84892DC`

Its sole file is `bin/NativeMods/FirstPersonExplorerNative.dll` (193536 bytes).
DLL SHA-256:
`B45AEC985A4CC21AC04C4E35D39A7FDDB25B119461E10896F59813188F88FF66`

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
The four-table update adds static equipment support used by the tested warhammer.

The DLL makes no network requests, opens no other processes, and does not read
credentials, browser data or personal files. It does modify rendering-table pointers
inside the game process, as described above. The main Lua scripts communicate
through BG3's client/server mod API and retain startup/error/fallback reporting;
routine public-release camera and scale traces are disabled.

These notes describe the implementation; they do not assert why a scanner flagged
the file or substitute for a moderator's review. The exact source is included, and
BUILD.md describes how to compile and run tests.
