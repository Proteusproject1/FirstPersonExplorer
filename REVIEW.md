# Native review notes: 3.0.0.0

Nexus mod: https://www.nexusmods.com/baldursgate3/mods/24895

## Released artifacts

Native archive `TrueFirstPersonCamera_Native_DX11_3.0.0.0_Vortex.zip`:
`4A90E5F3EC55F1B1FCE33BEFD3EAD25A1AB858B0786D04285CFFC66FE4F9A97D`

It contains `bin/NativeMods/FirstPersonExplorerNative.dll` (70656 bytes) and `MinHook-LICENSE.txt`.

DLL SHA-256: `5AD443990534CB06081872061B35EE3B1B3317A0D22D55F706171FF88A40D268`
([VirusTotal](https://www.virustotal.com/gui/file/5ad443990534cb06081872061b35ee3b1b3317a0d22d55f706171ff88a40d268))

DLL SHA-256 with the 4-byte PE timestamp zeroed (what `build-native.ps1` reproduces):
`4357C5282C7F3AE71E9DFEB1BB0F43AC993A6D8B66E50D94691DAB4E44431CD0`

Main archive `TrueFirstPersonCamera_3.0.0_Main_Vortex.zip`:
`7A7971C261F974A6926AA31C4363A4BA5238080633D7E1F5623C0550CAB87567`
(PAK `TrueFirstPersonCamera_3.0.0_Main.pak`: `7AB4243E5D7C000481C1CBA0F2E3A8AFCAEF7FAD09044B26F613EF96C31696F5`)

## Changes from 0.7.7.0

- **Full-body camera (`camera.c`).** Two MinHook entry hooks: the game's camera update and the
  zoom step it calls. Both are located by signature like the other hooked code (`locate.c`);
  if either is not found exactly once, only the camera feature is disabled and the rest of the
  DLL runs as in 0.7.7.0. See "Camera" below.
- **Eye fix, verified separately.** The eye-distance code inside the camera update
  (update+0xB23) must match exactly once; otherwise the eye fix alone is disabled.
- **Head tagging and camera capability** through the existing render bridge: the Lua tags the
  character's head meshes with a recognised scale-vector command, and can ask whether the camera
  hooks are installed. That question is answered only while they are; otherwise it is forwarded
  like any other scale call and the mod uses its classic Hidden body view.
- **Imports added:** `QueryPerformanceCounter`/`QueryPerformanceFrequency` (frame timing),
  `TryAcquireSRWLockShared` (the camera never blocks on the settings lock), and
  `DeleteFileW`/`MoveFileExW` (the camera log rolls over to one `.prev.log`).

## What the DLL does

Native Mod Loader loads the DLL into the BG3 DX11 process. A background thread locates the
hooked code and writes `FirstPersonExplorerNative.log` beside the DLL. The locator compares the
game's code instruction by instruction with the verified build's code. Only fields that change
whenever code moves are ignored: 32-bit relative call/branch targets and RIP-relative
displacements, decoded with MinHook's bundled `hde64`. Everything else must match. Only
committed, readable pages are scanned. Any mismatch disables the affected feature, and the log
names the failed check.

**Render bridge.** It uses `VirtualProtect` and atomic compare-exchange to replace three
vtable slots (scale, render, destructor) in the four located mesh tables, pins its module,
and rolls back its own hooks if installation partly fails. The companion Lua sends special
scale-vector commands through Script Extender. The DLL consumes recognised probe, hide, show,
head and camera-capability commands, and keeps a bounded, locked per-object registry with 500 ms
leases. Render calls for hidden objects are skipped; all other calls are forwarded to the
original functions.

**Instant scale.** A MinHook mid-function hook in the engine's scale-transition update lets
an authorised resize snap instead of animating. Authorisation comes from the mod's own Lua
through a small mailbox file in
`%LOCALAPPDATA%\Larian Studios\Baldur's Gate 3\Script Extender\FirstPersonExplorer`
(nonce, fixed-length records, 350 ms freshness, one-shot per request). The hook reads the
engine frame with `ReadProcessMemory` on its own process so a bad pointer cannot crash it,
and writes only the authorised scale value. Counters and samples go to
`FirstPersonExplorerScaleDiagnostic.log`.

**Camera.** The Lua writes its full-body settings to `fullbody_camera_0778.txt` in the same
Script Extender folder: one text record framed by `FPC5` tags with 23 integers, each range-checked.
A changed sequence number is the heartbeat; without a new one within 400 ms the camera is left
alone. After the game's zoom step, and only while full body is on, the heartbeat is fresh and the
identity check passes, the hook rewrites the camera's smoothed root (offsets 0x3C and 0x48 of the
camera object) so the final eye lands in the character's head. With the eye fix it also writes the
eye-distance field (0x15C). The head position is the union of the world bounds of the meshes the
Lua tagged, read with `ReadProcessMemory`. Identity: the tagged head must be within 3 m of the
character's position sent by the Lua (otherwise the camera root must match the Lua's reference
within 1.5 m), and non-finite or out-of-range camera values are refused. Anything else leaves the
game's camera untouched. `FirstPersonExplorerCamera.log` gets per-frame rows only while the mod's
diagnostic logs are switched on, and rolls over to `FirstPersonExplorerCamera.prev.log` after
50,000 rows.

**Imports** (KERNEL32 only): the bridge, camera and logging use SRW locks, `CreateFileW`,
`WriteFile`, `ReadFile`, `FlushFileBuffers`, `CreateDirectoryW`, `DeleteFileW`, `MoveFileExW`,
`GetEnvironmentVariableW`, `VirtualProtect`, `VirtualQuery`, `GetModuleHandle(Ex)W`,
`GetModuleFileNameW`, `CreateThread`, `GetTickCount64`, `QueryPerformanceCounter`,
`QueryPerformanceFrequency`, `Sleep`, `ReadProcessMemory`. The thread, context, heap and
toolhelp imports (`SuspendThread`, `GetThreadContext`, `CreateToolhelp32Snapshot`, `HeapAlloc`,
`VirtualAlloc`, `FlushInstructionCache`, ...) are MinHook's standard way of safely patching code
while other threads run.

The DLL makes no network requests, opens no other processes, and does not read credentials,
browser data or personal files. It does modify rendering-table pointers, one engine scale
function and two camera functions inside the game process, as described above.

A few antivirus heuristics flag the DLL (VirusTotal 2/71: Bkav Pro and Cynet, generic and
machine-learning detections; 0.7.7.0 was 4/71), as is common for small unsigned DLLs that hook
game code. These notes describe the implementation; they do not substitute for a moderator's
review. The exact source is included, and BUILD.md shows how to rebuild it and confirm it
matches the released binary.
