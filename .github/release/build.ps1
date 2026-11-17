# Builds one architecture of a wtmux release and packages it:
#
#   pwsh .github/release/build.ps1 -Arch x64|x86|arm64 [-OutDir dist] [-WorkDir out/release]
#
# Run it in a Visual Studio developer environment for that architecture (amd64,
# amd64_x86 or amd64_arm64), with VCPKG_ROOT set. It writes
# wtmux-<version>-windows-<arch>.zip and facts-<arch>.json (what finish.ps1
# needs) to OutDir. The zip is flat: tmux.exe, wtmux-service.exe, conpty.dll,
# <arch>\OpenConsole.exe, COPYING, README.md and THIRD-PARTY.txt. In a tag
# build (GITHUB_REF_TYPE=tag) the tag must be v<version>.
param(
    [Parameter(Mandatory)][ValidateSet('x64', 'x86', 'arm64')][string]$Arch,
    [string]$OutDir = 'dist',
    [string]$WorkDir = 'out/release'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $root
$OutDir = [IO.Path]::GetFullPath($OutDir)
$WorkDir = [IO.Path]::GetFullPath($WorkDir)

function Invoke-Checked([string]$What, [scriptblock]$Block) {
    & $Block
    if ($LASTEXITCODE) { throw "$What failed (exit $LASTEXITCODE)" }
}

function Get-CacheValue([string]$BuildDir, [string]$Name) {
    $line = Get-Content (Join-Path $BuildDir 'CMakeCache.txt') | Where-Object { $_ -like "${Name}:*" } | Select-Object -First 1
    if (-not $line) { throw "$Name is not in the CMake cache" }
    $line.Substring($line.IndexOf('=') + 1)
}

function Get-PeMachine([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    [BitConverter]::ToUInt16($bytes, $pe + 4)
}

$version = [regex]::Match((Get-Content -Raw CMakeLists.txt), 'project\(\s*\S+\s+VERSION\s+(\d+\.\d+\.\d+)').Groups[1].Value
if (-not $version) { throw 'CMakeLists.txt has no project(... VERSION x.y.z)' }
if ($env:GITHUB_REF_TYPE -eq 'tag' -and $env:GITHUB_REF_NAME -ne "v$version") {
    throw "the tag $env:GITHUB_REF_NAME is not v$version, the version in CMakeLists.txt"
}
if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is not set' }

$machines = @{ x64 = 0x8664; x86 = 0x14C; arm64 = 0xAA64 }
$spec = @{
    x64   = @{ Preset = 'msvc-release'; Triplet = 'x64-windows-static' }
    x86   = @{ Preset = 'msvc-x86-release'; Triplet = 'x86-windows-static' }
    arm64 = @{ Preset = 'msvc-arm64-release'; Triplet = 'arm64-windows-static' }
}[$Arch]
# One zip per CPU: each carries only its own OpenConsole.exe folder. conpty.dll starts the
# OpenConsole.exe of the machine's native CPU, so a zip run on another CPU (x64 on ARM64,
# under emulation) finds none and tmux uses the console host built into Windows instead.
$spec.Hosts = @($Arch)

$build = Join-Path $WorkDir "build-$Arch"
$stage = Join-Path $WorkDir "stage-$Arch"
foreach ($dir in $stage) { if (Test-Path $dir) { Remove-Item -Recurse -Force $dir } }
New-Item -ItemType Directory -Force $OutDir | Out-Null

# Build, then install into a stage directory. TMUX_WIN32_CONPTY=bundled makes a
# missing ConPTY package an error instead of a build without it.
Invoke-Checked 'configure' { cmake --preset $spec.Preset -B $build -DTMUX_WIN32_CONPTY=bundled }
Invoke-Checked 'build' { cmake --build $build }
Invoke-Checked 'install' { cmake --install $build --prefix $stage }
$pkg = Join-Path $stage 'bin'
# The build installs an OpenConsole.exe for every CPU this one can run on; the zip keeps its own.
foreach ($other in @('x64', 'x86', 'arm64') | Where-Object { $_ -ne $Arch }) {
    $dir = Join-Path $pkg $other
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
}

$conptyVersion = Get-CacheValue $build 'TMUX_WIN32_CONPTY_VERSION'
$minBuild = Get-CacheValue $build 'TMUX_WIN32_MIN_BUILD'
$compiler = [regex]::Match((Get-Content -Raw (Get-ChildItem (Join-Path $build 'CMakeFiles') -Recurse -Filter CMakeCCompiler.cmake | Select-Object -First 1).FullName),
    'set\(CMAKE_C_COMPILER_VERSION "([^"]+)"\)').Groups[1].Value
$tmuxVersion = [regex]::Match((Get-Content -Raw 'cmake/Version.cmake'), 'set\(TMUX_VERSION "([^"]+)"\)').Groups[1].Value
$protocol = [regex]::Match((Get-Content -Raw 'tmux-protocol.h'), '#define\s+PROTOCOL_VERSION\s+(\d+)').Groups[1].Value

# THIRD-PARTY.txt: the libraries linked in (their licence files as vcpkg
# installed them), the ConPTY package, and every distinct licence notice in
# the sources compiled into the executables.
$share = Join-Path $build "vcpkg_installed/$($spec.Triplet)/share"
$deps = [ordered]@{}
$text = [Text.StringBuilder]::new()
[void]$text.AppendLine("wtmux $version for Windows ($Arch): third-party notices")
[void]$text.AppendLine('')
[void]$text.AppendLine('tmux.exe and wtmux-service.exe are built from the tmux sources (see COPYING),')
[void]$text.AppendLine('with the libraries below linked in statically. conpty.dll and OpenConsole.exe')
[void]$text.AppendLine("are Microsoft's ConPTY package, redistributed unchanged.")
foreach ($port in 'libevent', 'utf8proc', 'libpng', 'zlib') {
    $spdx = Get-Content -Raw (Join-Path $share "$port/vcpkg.spdx.json") | ConvertFrom-Json
    $portVersion = ($spdx.packages | Where-Object name -eq $port | Select-Object -First 1).versionInfo -replace '#\d+$', ''
    $deps[$port] = $portVersion
    [void]$text.AppendLine('')
    [void]$text.AppendLine("==== $port $portVersion ====")
    [void]$text.AppendLine('')
    [void]$text.AppendLine((Get-Content -Raw (Join-Path $share "$port/copyright")).TrimEnd())
}
$deps['ConPTY'] = $conptyVersion
[void]$text.AppendLine('')
[void]$text.AppendLine("==== Microsoft.Windows.Console.ConPTY $conptyVersion (conpty.dll, OpenConsole.exe) ====")
[void]$text.AppendLine('')
[void]$text.AppendLine(@'
MIT License

Copyright (c) Microsoft Corporation. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
'@.TrimEnd())

$notices = [ordered]@{}
$sources = Get-ChildItem $root -Recurse -File -Include *.c, *.h, *.rc |
    Where-Object { $_.FullName -notlike "$WorkDir*" -and $_.FullName -notlike (Join-Path $root 'out') + '*' } |
    Sort-Object FullName
foreach ($file in $sources) {
    $rel = $file.FullName.Substring($root.Length + 1).Replace('\', '/')
    foreach ($m in [regex]::Matches([IO.File]::ReadAllText($file.FullName), '(?ms)^/\*.*?\*/')) {
        if ($m.Value -notmatch 'Copyright|Permission to use|Permission is hereby granted|Redistribution and use|Public domain') { continue }
        $body = ($m.Value -replace '^/\*\s*', '' -replace '\s*\*/$', '' -split "`n" |
            ForEach-Object { $_ -replace '^\s?\* ?', '' -replace '\s+$', '' }) -join "`n"
        $body = $body.Trim()
        if (-not $notices.Contains($body)) { $notices[$body] = [Collections.Generic.List[string]]::new() }
        $notices[$body].Add($rel)
    }
}
[void]$text.AppendLine('')
[void]$text.AppendLine("==== Notices in the sources of tmux.exe and wtmux-service.exe ($($notices.Count)) ====")
foreach ($entry in $notices.GetEnumerator()) {
    [void]$text.AppendLine('')
    [void]$text.AppendLine("---- $($entry.Value -join ', ')")
    [void]$text.AppendLine('')
    [void]$text.AppendLine($entry.Key)
}
[IO.File]::WriteAllText((Join-Path $pkg 'THIRD-PARTY.txt'), $text.ToString().Replace("`r`n", "`n"))
Copy-Item COPYING, README.md $pkg

# The zip holds exactly these files, built for the right machines, with the
# release number in the version resource and Microsoft's signature on ConPTY.
$expected = @('tmux.exe', 'wtmux-service.exe', 'conpty.dll', 'COPYING', 'README.md', 'THIRD-PARTY.txt') +
    @($spec.Hosts | ForEach-Object { "$_/OpenConsole.exe" })
$actual = @(Get-ChildItem $pkg -Recurse -File | ForEach-Object { $_.FullName.Substring($pkg.Length + 1).Replace('\', '/') })
$missing = @($expected | Where-Object { $_ -notin $actual })
$extra = @($actual | Where-Object { $_ -notin $expected })
if ($missing -or $extra) { throw "zip contents: missing [$($missing -join ', ')], unexpected [$($extra -join ', ')]" }
foreach ($exe in 'tmux.exe', 'wtmux-service.exe', 'conpty.dll') {
    $machine = Get-PeMachine (Join-Path $pkg $exe)
    if ($machine -ne $machines[$Arch]) { throw "$exe is built for machine 0x$('{0:X}' -f $machine), not $Arch" }
}
foreach ($hostArch in $spec.Hosts) {
    $machine = Get-PeMachine (Join-Path $pkg "$hostArch/OpenConsole.exe")
    if ($machine -ne $machines[$hostArch]) { throw "$hostArch/OpenConsole.exe is built for machine 0x$('{0:X}' -f $machine)" }
}
foreach ($exe in 'tmux.exe', 'wtmux-service.exe') {
    $info = (Get-Item (Join-Path $pkg $exe)).VersionInfo
    if ($info.ProductVersion -ne $version -or $info.FileVersion -ne "$version.0") {
        throw "$exe has version $($info.ProductVersion) / $($info.FileVersion), not $version"
    }
}
foreach ($ms in @('conpty.dll') + @($spec.Hosts | ForEach-Object { "$_/OpenConsole.exe" })) {
    $sig = Get-AuthenticodeSignature (Join-Path $pkg $ms)
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
        throw "$ms is not validly signed by Microsoft ($($sig.Status))"
    }
}

$zipName = "wtmux-$version-windows-$Arch.zip"
$zip = Join-Path $OutDir $zipName
if (Test-Path $zip) { Remove-Item -Force $zip }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($pkg, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)

$facts = [ordered]@{
    version = $version; arch = $Arch; zip = $zipName
    size = (Get-Item $zip).Length; sha256 = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
    tmux = $tmuxVersion; protocol = [int]$protocol; minimumWindowsBuild = [int]$minBuild
    compiler = "MSVC $compiler"; dependencies = $deps
}
[IO.File]::WriteAllText((Join-Path $OutDir "facts-$Arch.json"), (($facts | ConvertTo-Json -Depth 4) + "`n").Replace("`r`n", "`n"))
"$zipName  $($facts.size) bytes  sha256 $($facts.sha256)"
