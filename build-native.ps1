param([string]$Zig = 'zig')
$ErrorActionPreference = 'Stop'
$compiler = (Get-Command $Zig -ErrorAction Stop).Source
$version = (& $compiler version).Trim()
if ($LASTEXITCODE -ne 0 -or $version -ne '0.16.0') { throw 'Use Zig 0.16.0 for this release.' }
# The DLL links no C runtime, so Zig's bundled Windows headers are passed explicitly.
$headers = Join-Path (Split-Path $compiler) 'lib/libc/include/any-windows-any'
if (-not (Test-Path $headers)) { throw "Windows headers not found at $headers" }
Push-Location $PSScriptRoot
try {
    & $compiler cc public-release-0763/native/test_render.c -O2 -Wall -Wextra -Werror -o public-release-0763/native/test_render.exe
    if ($LASTEXITCODE -ne 0) { throw 'Native test compilation failed' }
    & ./public-release-0763/native/test_render.exe
    if ($LASTEXITCODE -ne 0) { throw 'Native tests failed' }
    & $compiler cc public-release-0763/native/fpe_render.c public-release-0763/native/fpe_version.rc -target x86_64-windows-gnu -isystem $headers -shared -O2 -Wall -Wextra -Werror -nostdlib -ffreestanding -lkernel32 -s -o public-release-0763/native/FirstPersonExplorerNative.dll
    if ($LASTEXITCODE -ne 0) { throw 'Native compilation failed' }
    $stage = Join-Path $PSScriptRoot ('build/native-' + [Guid]::NewGuid().ToString('N'))
    $folder = Join-Path $stage 'bin/NativeMods'
    New-Item -ItemType Directory -Force -Path $folder,dist | Out-Null
    Copy-Item -LiteralPath public-release-0763/native/FirstPersonExplorerNative.dll -Destination $folder
    $zip = Join-Path $PSScriptRoot 'dist/FirstPersonExplorer_0.7.6.7_Native_DX11_Vortex.zip'
    Compress-Archive -LiteralPath (Join-Path $stage 'bin') -DestinationPath $zip -Force
    Get-FileHash -LiteralPath public-release-0763/native/FirstPersonExplorerNative.dll,$zip -Algorithm SHA256
} finally { Pop-Location }
