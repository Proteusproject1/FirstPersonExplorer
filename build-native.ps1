param(
    [string]$Zig = 'zig',
    # Optional: the locator test maps this executable read-only. Skipped if the file is absent.
    [string]$GameExe = 'C:\Program Files (x86)\Steam\steamapps\common\Baldurs Gate 3\bin\bg3_dx11.exe'
)
# True First-Person Camera Native DX11 3.0.0.0: tests, DLL and Vortex ZIP.
$ErrorActionPreference = 'Stop'
$compiler = (Get-Command $Zig -ErrorAction Stop).Source
$version = (& $compiler version).Trim()
if ($LASTEXITCODE -ne 0 -or $version -ne '0.16.0') { throw 'Use Zig 0.16.0 for this release.' }
# The DLL links no C runtime, so Zig's bundled Windows headers are passed explicitly.
$headers = Join-Path (Split-Path $compiler) 'lib/libc/include/any-windows-any'
if (-not (Test-Path $headers)) { throw "Windows headers not found at $headers" }
$native = Join-Path $PSScriptRoot 'release-3.0.0/native'
$vendor = Join-Path $native 'vendor/minhook'
$mh = @("$vendor/src/hook.c", "$vendor/src/buffer.c", "$vendor/src/trampoline.c", "$vendor/src/hde/hde64.c")
$work = Join-Path $PSScriptRoot ('build/native-' + [Guid]::NewGuid().ToString('N'))
$dist = Join-Path $PSScriptRoot 'dist'
New-Item -ItemType Directory -Force -Path $work, $dist | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $work 'zig-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $work 'zig-local'
Push-Location $PSScriptRoot
try {
    & $compiler cc "$native/test_render.c" -O2 -Wall -Wextra -Werror -o "$work/test_render.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Render test compilation failed' }
    & "$work/test_render.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Render tests failed' }
    & $compiler cc "$native/test_scale_diag.c" "$native/scale_diag_hook.S" "$native/test_scale_engine.S" @mh -O2 -Wall -Wextra -Werror -o "$work/test_scale_diag.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Instant scale test compilation failed' }
    & "$work/test_scale_diag.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Instant scale tests failed' }
    & $compiler cc "$native/test_camera.c" @mh -O2 -Wall -Wextra -Werror -o "$work/test_camera.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Camera test compilation failed' }
    & "$work/test_camera.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Camera tests failed' }
    if (Test-Path -LiteralPath $GameExe) {
        # Maps the game executable like the Windows loader (read-only file access) and checks
        # the locator finds the hooked code, then that tampered copies are refused. The expected
        # addresses are those of game build stamp 1789998461 (the 2026-09-29 hotfix).
        & $compiler cc "$native/test_locate.c" "$vendor/src/hde/hde64.c" -O2 -Wall -Wextra -Werror -o "$work/test_locate.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Locator test compilation failed' }
        & "$work/test_locate.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Locator tests failed (expected on a different game build)' }
    } else { Write-Output "Locator test skipped: $GameExe not found." }
    # Zig's GNU frontend auto-exports C symbols, so objects are linked with its COFF linker.
    $objects = @()
    foreach ($unit in @("$native/fpe_render.c", "$native/scale_diag.c", "$native/scale_diag_hook.S", "$native/locate.c", "$native/camera.c", "$native/freestanding.c") + $mh) {
        $object = Join-Path $work ([IO.Path]::GetFileNameWithoutExtension($unit) + '.obj')
        & $compiler cc -target x86_64-windows-gnu -isystem $headers -O2 -Wall -Wextra -Werror -ffreestanding -c $unit -o $object
        if ($LASTEXITCODE -ne 0) { throw "Object compilation failed: $unit" }
        $objects += $object
    }
    # A throwaway build-lib run supplies the x86_64 KERNEL32 import library in the Zig cache.
    & $compiler build-lib -target x86_64-windows-gnu -isystem $headers -dynamic -O ReleaseFast -fno-dll-export-fns -fentry=_DllMainCRTStartup -fno-compiler-rt -fno-ubsan-rt -lkernel32 -fstrip "-femit-bin=$work/import-probe.dll" -cflags -O2 -Wall -Wextra -Werror -ffreestanding -- "$native/fpe_render.c" "$native/scale_diag.c" "$native/scale_diag_hook.S" "$native/locate.c" "$native/camera.c" "$native/freestanding.c" @mh
    if ($LASTEXITCODE -ne 0) { throw 'Import library probe failed' }
    $resource = Join-Path $work 'fpe_version.res'
    & $compiler rc /i $headers /fo $resource "$native/fpe_version.rc"
    if ($LASTEXITCODE -ne 0) { throw 'Version resource compilation failed' }
    $kernel = @(Get-ChildItem -LiteralPath $env:ZIG_GLOBAL_CACHE_DIR -Recurse -Filter kernel32.lib -File)
    if ($kernel.Count -ne 1) { throw 'Ambiguous KERNEL32 import library' }
    $dll = Join-Path $dist 'FirstPersonExplorerNative.dll'
    & $compiler lld-link /dll /nodefaultlib /entry:_DllMainCRTStartup /machine:x64 /dynamicbase /nxcompat /opt:ref /opt:icf /noimplib '-exclude-all-symbols' "/out:$dll" @objects $resource $kernel[0].FullName
    if ($LASTEXITCODE -ne 0) { throw 'Link failed' }
    # The linker writes the build time into the PE header. With those 4 bytes zeroed, a rebuild
    # must match the released DLL exactly (see REVIEW.md).
    $bytes = [IO.File]::ReadAllBytes($dll)
    $stamp = [BitConverter]::ToInt32($bytes, 0x3c) + 8
    for ($i = 0; $i -lt 4; $i++) { $bytes[$stamp + $i] = 0 }
    $normalized = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($bytes)) -replace '-', ''
    $zip = Join-Path $dist 'TrueFirstPersonCamera_Native_DX11_3.0.0.0_Vortex.zip'
    if (Test-Path $zip) { Remove-Item -LiteralPath $zip }
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $stream = [IO.File]::Open($zip, [IO.FileMode]::CreateNew)
    $archive = New-Object IO.Compression.ZipArchive($stream, [IO.Compression.ZipArchiveMode]::Create)
    try {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $dll, 'bin/NativeMods/FirstPersonExplorerNative.dll', [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, (Join-Path $vendor 'LICENSE.txt'), 'MinHook-LICENSE.txt', [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    } finally { $archive.Dispose(); $stream.Dispose() }
    Get-FileHash -LiteralPath $dll, $zip -Algorithm SHA256
    Write-Output "DLL SHA-256 with the PE timestamp zeroed: $normalized"
    if ($normalized -eq '4357C5282C7F3AE71E9DFEB1BB0F43AC993A6D8B66E50D94691DAB4E44431CD0') { Write-Output 'Matches the released 3.0.0.0 DLL (apart from its build timestamp).' }
    else { Write-Output 'Does NOT match the released 3.0.0.0 DLL.' }
} finally {
    Pop-Location
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
