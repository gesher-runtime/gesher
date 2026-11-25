# Probe the installed production APK/APKS/XAPK icon path without installing a package
# or changing Explorer registration. All runtime writes go to OutputRoot.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $RuntimeDir,
    [Parameter(Mandatory)][string] $Apk,
    [Parameter(Mandatory)][string] $OutputRoot,
    [string] $ShellExtension
)
$ErrorActionPreference = 'Stop'
$RuntimeDir = (Resolve-Path -LiteralPath $RuntimeDir).Path
if (-not $ShellExtension) { $ShellExtension = Join-Path $RuntimeDir 'gesher_shellext.dll' }
$ShellExtension = (Resolve-Path -LiteralPath $ShellExtension).Path
$Apk = (Resolve-Path -LiteralPath $Apk).Path
$work = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('apk-icon-' + [guid]::NewGuid().ToString('N'))
$local = Join-Path $work 'local'
$extension = Join-Path $work 'extension'
New-Item -ItemType Directory -Force $local, $extension | Out-Null
# Reuse the native COM probe used by the shell track, without registering a DLL.
$probe = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'tracks/55-shell.ps1') -Raw
$begin = $probe.IndexOf('using System;', $probe.IndexOf('GesherShellExtProbe.Handler'))
$end = $probe.IndexOf("'@", $begin)
if ($begin -lt 0 -or $end -lt 0) { throw 'Existing shell COM probe missing' }
if (-not ('GesherShellExtProbe.Handler' -as [type])) {
    Add-Type -TypeDefinition $probe.Substring($begin, $end - $begin)
}
Copy-Item -LiteralPath $ShellExtension -Destination (Join-Path $extension 'gesher_shellext.dll')
$savedLocal = $env:LOCALAPPDATA
try {
    $env:LOCALAPPDATA = $local
    $process = Start-Process -FilePath (Join-Path $RuntimeDir 'gesher-launch.exe') `
        -ArgumentList @('apk-icon', ('"' + $Apk + '"')) -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(180000)) {
        $process.Kill()
        throw "Icon extraction timed out; evidence: $work"
    }
    if ($process.ExitCode -ne 0) { throw "Icon extraction failed ($($process.ExitCode)); evidence: $work" }
    $flags = [uint32]0
    $icon = [GesherShellExtProbe.Handler]::IconOf((Join-Path $extension 'gesher_shellext.dll'), $Apk, [ref]$flags)
    if (-not (Test-Path -LiteralPath $icon -PathType Leaf) -or
        [IO.Path]::GetFileName($icon) -eq 'gesher.ico' -or ($flags -band 2) -eq 0) {
        throw "Native handler did not return an application icon: $icon; evidence: $work"
    }
    Add-Type -AssemblyName System.Drawing
    $decoded = [Drawing.Icon]::new($icon)
    try { if ($decoded.Width -le 0) { throw 'Invalid icon dimensions' } }
    finally { $decoded.Dispose() }
    $installed = @(Get-ChildItem -LiteralPath (Join-Path $local 'Gesher/data/app') -Filter '*.apk' -Recurse -ErrorAction SilentlyContinue)
    if ($installed.Count) { throw 'Icon extraction unexpectedly installed APKs' }
    $receipt = [ordered]@{
        passed = $true; scope = 'Real production extraction and direct COM lookup; isolated profile; no Explorer registration or visual Explorer assertion'
        apk = $Apk; apkSha256 = (Get-FileHash -LiteralPath $Apk).Hash
        runtime = $RuntimeDir; shellExtension = $ShellExtension; dllSha256 = (Get-FileHash -LiteralPath $ShellExtension).Hash
        launcherSha256 = (Get-FileHash -LiteralPath (Join-Path $RuntimeDir 'gesher-launch.exe')).Hash
        appJarSha256 = (Get-FileHash -LiteralPath (Join-Path $RuntimeDir 'gesher-app.jar')).Hash
        icon = $icon; iconSha256 = (Get-FileHash -LiteralPath $icon).Hash
        flags = $flags; installedApks = $installed.Count
    }
    $receipt | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $work 'validation.json') -Encoding utf8
    Write-Output "PASS: application icon extracted and returned by COM without installation. Evidence: $work"
} finally { $env:LOCALAPPDATA = $savedLocal }
