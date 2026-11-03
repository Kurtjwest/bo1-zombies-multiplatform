# setup.ps1 - copy your Call of Duty: Black Ops install next to the built game (one time, about 11 GB).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File setup.ps1 [-GameDir "D:\Games\Call of Duty Black Ops"] [-Build <folder>]
#
# The game runs from its build folder and reads the original game's files (zone\, main\ with the .iwd files and the
# videos, localization.txt) from there: build\Release for the Visual Studio project, build-windows-x64 or
# build-windows-x86 for build-windows.sh (-Build picks one; without it the first of those that holds BO1Zombies.exe).
# Your Steam install is only read, never changed. Only what the game reads is copied: zone\, main\, mods\ (if the
# install has one) and localization.txt; the rest of the install (BlackOps.exe and its other programs, its DLLs: the
# build has its own, Redist\: the DirectX DLLs below are taken out of it, Steam's, the uninstaller's and other
# launchers' files, the mod tools' folders, players\) is left out. What an earlier version of this script copied
# there that the game has no use for (Redist\, the uninstaller's and launchers' files, ...) is removed from it.
param(
    [string]$GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\Call of Duty Black Ops',
    [string]$Build = ''
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path (Join-Path $GameDir 'zone'))) {
    throw "No Black Ops install at '$GameDir'. Pass -GameDir with the folder that holds BlackOps.exe."
}
if (-not $Build) {
    $Build = 'build\Release'
    foreach ($b in 'build\Release', 'build-windows-x64', 'build-windows-x86') {
        if (Test-Path (Join-Path (Join-Path $PSScriptRoot $b) 'BO1Zombies.exe')) { $Build = $b; break }
    }
}
$dst = if ([System.IO.Path]::IsPathRooted($Build)) { $Build } else { Join-Path $PSScriptRoot $Build }
New-Item -ItemType Directory -Force $dst | Out-Null

# which game is built there: 64-bit or 32-bit (the executable's PE machine), and whether it is the Visual Studio one
# (which needs the Visual C++ runtime; build-windows.sh's has its runtime linked in)
$exe = Join-Path $dst 'BO1Zombies.exe'
$x64 = $false
$msvc = $true
if (Test-Path $exe) {
    $bytes = [System.IO.File]::ReadAllBytes($exe)
    $pe = [System.BitConverter]::ToInt32($bytes, 0x3C)
    $x64 = [System.BitConverter]::ToUInt16($bytes, $pe + 4) -eq 0x8664
    $msvc = [System.Text.Encoding]::ASCII.GetString($bytes).Contains('VCRUNTIME140')
}

# what the game reads of the install (players\, your own profile and settings, is left out: the game makes a fresh one)
foreach ($d in 'zone', 'main', 'mods') {
    $from = Join-Path $GameDir $d
    if (-not (Test-Path $from -PathType Container)) { continue }
    robocopy $from (Join-Path $dst $d) *.* /E /NDL /NFL /NP /R:1 /W:1
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
}
$loc = Join-Path $GameDir 'localization.txt'
if (Test-Path $loc -PathType Leaf) { Copy-Item $loc $dst -Force }

# the DirectX cabinets: in the install's Redist\DirectX (_Redist\DirectX in some copies), or in the copy of it an
# earlier version of this script made next to the exe
$dxDirs = foreach ($root in $GameDir, $dst) { foreach ($r in 'Redist', '_Redist') { Join-Path (Join-Path $root $r) 'DirectX' } }
function Expand-Redist([string]$cab, [string]$file) {
    $path = $dxDirs | ForEach-Object { Join-Path $_ $cab } | Where-Object { Test-Path $_ -PathType Leaf } | Select-Object -First 1
    if ($path) { expand.exe $path "-F:$file" $dst | Out-Null }
    if (-not (Test-Path (Join-Path $dst $file))) {
        throw "could not extract $($file) from $($cab) (in the install's Redist\DirectX): install the DirectX End-User Runtime (June 2010), or put $($file) next to BO1Zombies.exe"
    }
}
if ($x64) {
    # 64-bit: steam_api64.dll is the build's own; D3DX9 (d3dx9_43) out of the game's Redist\DirectX (XInput is
    # Windows' own xinput1_4)
    Expand-Redist 'Jun2010_d3dx9_43_x64.cab' 'd3dx9_43.dll'
} else {
    # the engine's own DLLs replace the install's older ones (the exe is built against this Steam SDK and Bink)
    Copy-Item (Join-Path $PSScriptRoot 'src\binklib\binkw32.dll') $dst -Force
    Copy-Item (Join-Path $PSScriptRoot 'src\steam\redistributable_bin\*.dll') $dst -Force
    # runtimes the exe needs that a fresh Windows lacks, put next to the exe so nothing has to be installed:
    # DirectX 9 (d3dx9_43, xinput1_3) out of the game's own Redist\DirectX
    Expand-Redist 'Jun2010_d3dx9_43_x86.cab' 'd3dx9_43.dll'
    Expand-Redist 'APR2007_xinput_x86.cab' 'xinput1_3.dll'
}
if ($msvc -and -not $x64) {
    # Visual C++ 2015-2022 (x86) out of Visual Studio, for the Visual Studio project's build
    $crt = Get-ChildItem 'C:\Program Files*\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x86\Microsoft.VC14*.CRT' -Directory -ErrorAction SilentlyContinue |
        Sort-Object FullName | Select-Object -Last 1
    if ($crt) { Copy-Item (Join-Path $crt.FullName 'msvcp140.dll'), (Join-Path $crt.FullName 'vcruntime140.dll') $dst -Force }
    else { 'Visual C++ runtime not found next to Visual Studio: whoever plays needs the "Visual C++ Redistributable 2015-2022 (x86)"' }
}
# what an earlier version of this script copied there that the game has no use for: the install's Redist\ (its
# DirectX DLLs are taken out of it above), the uninstaller's, Steam's and other launchers' files, the splash pictures.
# (Not its programs and DLLs: the build's own are next to them.)
$leftovers = Get-ChildItem -LiteralPath $dst -Force | Where-Object {
    $_.Name -in 'Redist', '_Redist', '_CommonRedist', 'NoDVD' -or $_.Name -like 'unins*' -or
    ((-not $_.PSIsContainer) -and $_.Extension -in '.cdx', '.vdf', '.ini', '.nfo', '.url', '.bmp', '.ico')
}
if ($leftovers) {
    "Removing from $dst what an earlier setup copied there that the game has no use for:"
    foreach ($l in $leftovers) {
        "  $($l.Name)"
        Remove-Item -LiteralPath $l.FullName -Recurse -Force
    }
}
"game files ready in $dst"
