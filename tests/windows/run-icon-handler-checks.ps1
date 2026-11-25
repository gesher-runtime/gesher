# Build and exercise the production handler without COM registration or Android.
[CmdletBinding(DefaultParameterSetName = 'Build')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Build')][string] $Compiler,
    [Parameter(Mandatory, ParameterSetName = 'Prebuilt')][string] $ProbeDir,
    [Parameter(Mandatory)][string] $AppIcon,
    [Parameter(Mandatory)][string] $OutputRoot
)
$ErrorActionPreference = 'Stop'
if ($PSCmdlet.ParameterSetName -eq 'Build') { $Compiler = (Resolve-Path -LiteralPath $Compiler).Path }
else { $ProbeDir = (Resolve-Path -LiteralPath $ProbeDir).Path }
$AppIcon = (Resolve-Path -LiteralPath $AppIcon).Path
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$work = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $work | Out-Null
$brand = Join-Path $repo 'tools/package/gesher.ico'
if (-not (Test-Path -LiteralPath $brand)) { throw "Missing product icon: $brand" }
Copy-Item -LiteralPath $brand -Destination (Join-Path $work 'gesher.ico')
foreach ($name in @('icon_async_probe', 'icon_badge_probe')) {
    $source = Join-Path $PSScriptRoot ($name + '.cc')
    $exe = Join-Path $work ($name + '.exe')
    if ($PSCmdlet.ParameterSetName -eq 'Build') {
        & $Compiler '-std=c++17' '-Wall' '-Wextra' '-Werror' '-fno-exceptions' '-fno-rtti' '-static' '-municode' $source '-o' $exe '-lole32' '-luser32' '-lgdi32'
        if ($LASTEXITCODE -ne 0) { throw "Build failed: $name" }
    } else {
        Copy-Item -LiteralPath (Join-Path $ProbeDir ($name + '.exe')) -Destination $exe
    }
}
& (Join-Path $work 'icon_badge_probe.exe') $AppIcon
if ($LASTEXITCODE -ne 0) { throw 'Badge contract failed' }
$local = Join-Path $work 'local'
$system = Join-Path $local 'Gesher/data/system'
New-Item -ItemType Directory -Force $system | Out-Null
$apk = Join-Path $work 'fixture.apk'
# Deliberately synthetic: this test publishes a cache entry, never parses an APK.
[IO.File]::WriteAllBytes($apk, [byte[]]@(1, 2, 3, 4))
$savedLocal = $env:LOCALAPPDATA
try {
    $env:LOCALAPPDATA = $local
    & (Join-Path $work 'icon_async_probe.exe') $apk (Join-Path $system 'apk-icons.list') $AppIcon
    if ($LASTEXITCODE -ne 0) { throw 'Async contract failed' }
} finally {
    $env:LOCALAPPDATA = $savedLocal
}
Write-Output "PASS: isolated native icon contracts; evidence directory: $work"
