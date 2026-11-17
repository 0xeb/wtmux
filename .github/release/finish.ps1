# Completes a wtmux release from the per-architecture outputs of build.ps1:
#
#   pwsh .github/release/finish.ps1 [-InDir dist] [-OutDir release] [-Arches x64,x86,arm64]
#
# OutDir receives the release assets, the zips, SHA256SUMS.txt
# ("<sha256>  <file>", one line per zip) and wtmux-<version>-release.json
# (version, tmux version, protocol version, minimum Windows build and each
# zip's architecture, size and SHA-256), plus release-notes.md, the text of the
# draft release.
param(
    [string]$InDir = 'dist',
    [string]$OutDir = 'release',
    [string[]]$Arches = @('x64', 'x86', 'arm64')
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3

$InDir = [IO.Path]::GetFullPath($InDir)
$OutDir = [IO.Path]::GetFullPath($OutDir)
if (Test-Path $OutDir) { Remove-Item -Recurse -Force $OutDir }
New-Item -ItemType Directory $OutDir | Out-Null

function Write-Lf([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text.Replace("`r`n", "`n"))
}

$facts = foreach ($arch in $Arches) {
    $path = Join-Path $InDir "facts-$arch.json"
    if (-not (Test-Path $path)) { throw "no build for $arch ($path)" }
    $f = Get-Content -Raw $path | ConvertFrom-Json
    $zip = Join-Path $InDir $f.zip
    if ((Get-Item $zip).Length -ne $f.size) { throw "$($f.zip): size differs from its build" }
    if ((Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant() -ne $f.sha256) { throw "$($f.zip): SHA-256 differs from its build" }
    Copy-Item $zip $OutDir
    $f
}
$first = $facts[0]
foreach ($f in $facts) {
    foreach ($key in 'version', 'tmux', 'protocol', 'minimumWindowsBuild') {
        if ($f.$key -ne $first.$key) { throw "the builds disagree on $key ($($f.$key) vs $($first.$key))" }
    }
}
$version = $first.version

$sums = ($facts | Sort-Object zip | ForEach-Object { "$($_.sha256)  $($_.zip)" }) -join "`n"
Write-Lf (Join-Path $OutDir 'SHA256SUMS.txt') "$sums`n"

$release = [ordered]@{
    schema              = 1
    name                = 'wtmux'
    version             = $version
    tag                 = "v$version"
    tmux                = $first.tmux
    protocol            = $first.protocol
    minimumWindowsBuild = $first.minimumWindowsBuild
    checksums           = 'SHA256SUMS.txt'
    assets              = @($facts | ForEach-Object { [ordered]@{ arch = $_.arch; name = $_.zip; size = $_.size; sha256 = $_.sha256 } })
}
Write-Lf (Join-Path $OutDir "wtmux-$version-release.json") (($release | ConvertTo-Json -Depth 4) + "`n")

$byArch = @{}
foreach ($f in $facts) { $byArch[$f.arch] = $f }
$labels = [ordered]@{ x64 = 'x64 (64-bit Intel/AMD)'; arm64 = 'ARM64 (Snapdragon and other ARM PCs)'; x86 = 'x86 (32-bit Windows)' }
$notes = [Text.StringBuilder]::new()
[void]$notes.AppendLine("tmux for Windows, built from tmux $($first.tmux) with Visual Studio. One self-contained folder; Windows 10 1809 (build $($first.minimumWindowsBuild)) or later.")
[void]$notes.AppendLine('')
[void]$notes.AppendLine('## Download')
[void]$notes.AppendLine('')
[void]$notes.AppendLine('| Windows | File |')
[void]$notes.AppendLine('|---|---|')
foreach ($label in $labels.GetEnumerator()) {
    if ($byArch.ContainsKey($label.Key)) { [void]$notes.AppendLine("| $($label.Value) | ``$($byArch[$label.Key].zip)`` |") }
}
[void]$notes.AppendLine('')
[void]$notes.AppendLine('Take the zip for your PC''s processor (Settings > System > About > System type).')
[void]$notes.AppendLine('')
[void]$notes.AppendLine('Extract the zip and put the folder on your `PATH`. For `wtmux-service`, extract it where only')
[void]$notes.AppendLine('administrators can change files, such as `C:\Program Files\wtmux`, and run')
[void]$notes.AppendLine('`wtmux-service install` from an elevated prompt.')
[void]$notes.AppendLine('')
[void]$notes.AppendLine('Check a download against `SHA256SUMS.txt` with `Get-FileHash <zip>`. `tmux -V -V` shows the release.')
[void]$notes.AppendLine('')
[void]$notes.AppendLine('## Built with')
[void]$notes.AppendLine('')
[void]$notes.AppendLine('| Component | Version |')
[void]$notes.AppendLine('|---|---|')
[void]$notes.AppendLine("| tmux | $($first.tmux) |")
foreach ($dep in $first.dependencies.PSObject.Properties) { [void]$notes.AppendLine("| $($dep.Name) | $($dep.Value) |") }
[void]$notes.AppendLine("| Compiler | $($first.compiler) |")
Write-Lf (Join-Path $OutDir 'release-notes.md') $notes.ToString()

Get-ChildItem $OutDir | ForEach-Object { '{0,-40} {1,10}' -f $_.Name, $_.Length }
