# Native review notes: 0.7.7.0

Nexus mod: https://www.nexusmods.com/baldursgate3/mods/24895

## Released artifacts

Native archive `TrueFirstPersonCamera_Native_DX11_0.7.7.0_Vortex.zip`:
`A0DD6B641191C01B1F9337B9B2EB4B719DA7A496AF5B81E563809FFCD8631EA6`

It contains `bin/NativeMods/FirstPersonExplorerNative.dll` (48640 bytes) and `MinHook-LICENSE.txt`.

DLL SHA-256: `B8440048560B87D63522236174FA2B674A730ED83C87BA1F4EBBD8A7A5C225D9`
([VirusTotal](https://www.virustotal.com/gui/file/b8440048560b87d63522236174fa2b674a730ed83c87ba1f4ebbd8a7a5c225d9))

DLL SHA-256 with the 4-byte PE timestamp zeroed (what `build-native.ps1` reproduces):
`31D8F9EAD9F91E0CB7474C716513E2054621D82E26086C7C2910A4FA34FA6326`

Main archive `TrueFirstPersonCamera_0.9.0_Main_Vortex.zip`:
`08CCAFF22AD05DE22AD48ED8ABECA2D4C806023534A29FDAC3E0AD3C54FD8609`
(PAK `TrueFirstPersonCamera_0.9.0_Main.pak`: `D1032D2F28EA71DF35B065E6CF392BA85788DB354420976A9ED917682BDDF589`)

## Changes from 0.7.6.7

- **Hooked code located by signature (`locate.c`).** 0.7.6.7 ran only on the exact game build
  it was verified against (timestamp and image-size check, fixed addresses). A game hotfix
  therefore disabled it. 0.7.7.0 searches the game's own code for the functions it hooks,
  comparing instruction by instruction with the verified build's code. Only fields that
  change whenever code moves are ignored (32-bit relative call/branch targets and
  RIP-relative displacements, decoded with MinHook's bundled `hde64`). Everything else must
  match: each function exactly once, exactly the 4 render-object tables with the verified
  4x30 slot layout and destructor code, and the full instant-scale function including the
  stack-frame offsets the hook reads and the exact instructions at the hook point. Only
  committed, readable pages are scanned. Any mismatch disables the affected feature and the
  log names the failed check.
- **Instant scale** (added after 0.7.6.7, in 0.7.6.9): see below.

## What the DLL does

Native Mod Loader loads the DLL into the BG3 DX11 process. A background thread locates the
hooked code as described above and writes `FirstPersonExplorerNative.log` beside the DLL.

**Render bridge.** It uses `VirtualProtect` and atomic compare-exchange to replace three
vtable slots (scale, render, destructor) in the four located mesh tables, pins its module,
and rolls back its own hooks if installation partly fails. The companion Lua sends special
scale-vector commands through Script Extender; the DLL consumes recognised probe/hide/show
commands and keeps a bounded, locked per-object registry with 500 ms leases. Render calls for
registered objects are skipped; all other calls are forwarded to the original functions.

**Instant scale.** A MinHook mid-function hook in the engine's scale-transition update lets
an authorised resize snap instead of animating. Authorisation comes from the mod's own Lua
through a small mailbox file in
`%LOCALAPPDATA%\Larian Studios\Baldur's Gate 3\Script Extender\FirstPersonExplorer`
(nonce, fixed-length records, 350 ms freshness, one-shot per request). The hook reads the
engine frame with `ReadProcessMemory` on its own process so a bad pointer cannot crash it,
and writes only the authorised scale value. Counters and samples go to
`FirstPersonExplorerScaleDiagnostic.log`.

**Imports** (KERNEL32 only): the render bridge and logging use SRW locks, `CreateFileW`,
`WriteFile`, `ReadFile`, `FlushFileBuffers`, `CreateDirectoryW`, `GetEnvironmentVariableW`,
`VirtualProtect`, `VirtualQuery`, `GetModuleHandle(Ex)W`, `GetModuleFileNameW`, `CreateThread`,
`GetTickCount64`, `Sleep`, `ReadProcessMemory`. The thread, context, heap and toolhelp imports
(`SuspendThread`, `GetThreadContext`, `CreateToolhelp32Snapshot`, `HeapAlloc`, `VirtualAlloc`,
`FlushInstructionCache`, ...) are MinHook's standard way of safely patching code while other
threads run.

The DLL makes no network requests, opens no other processes, and does not read credentials,
browser data or personal files. It does modify rendering-table pointers and one engine
function inside the game process, as described above.

A few machine-learning antivirus heuristics flag the DLL (VirusTotal 4/71, the same engines
and labels as 0.7.6.9), as is common for small unsigned DLLs that hook game code. These notes
describe the implementation; they do not substitute for a moderator's review. The exact source
is included, and BUILD.md shows how to rebuild it and confirm it matches the released binary.
