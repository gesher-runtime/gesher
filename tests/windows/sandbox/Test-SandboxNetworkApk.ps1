# Keep the operator's HTTP(S) fixtures running throughout this qualification.
# The APK must be built with independent public and same-PC endpoints.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $RuntimeDir,
    [Parameter(Mandatory)][string] $Apk,
    [Parameter(Mandatory)][string] $ScratchDir,
    [switch] $WithLoopback,
    [switch] $WithLanPeer
)

$ErrorActionPreference = 'Stop'
$Apk = (Resolve-Path -LiteralPath $Apk).Path
$runtime = (Resolve-Path -LiteralPath $RuntimeDir).Path
$run = Join-Path ([IO.Path]::GetFullPath($ScratchDir)) ('network-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $run -Force | Out-Null
$probe = Join-Path $PSScriptRoot 'AppContainerPoc.ps1'
$local = @('host')
if ($WithLoopback) { $local += 'loopback' }
if ($WithLanPeer) { $local += 'lan' }

function RunProbe([string] $Mode, [bool] $Ordinary) {
    $log = Join-Path $run "$Mode.log"
    $launch = '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $probe, '-Dir',
        $runtime, '-Apk', $Apk, '-Root', (Join-Path $run $Mode), '-HiddenWindow', '-Seconds', '100'
    if ($Ordinary) { $launch += '-NoSandbox' }
    $priorPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & powershell.exe @launch *> $log
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $priorPreference
    }
    if ($exitCode -ne 0) { throw "$Mode setup or launch failed. See $log" }
    $result = Get-Content -LiteralPath $log -Raw
    if ($result -notmatch 'sandbox-network-apk: completed') { throw "$Mode probe did not finish. See $log" }
    if (!$Ordinary) {
        if ($result -notmatch '(?m)^pid \d+\s*: AppContainer ') { throw "Missing APK token observation. See $log" }
        if ($result -notmatch 'sandbox: com\.gesher\.sandboxnetwork .*with \[gesherRuntimeFiles, gesherBinderClient, internetClient\]') {
            throw "Missing Internet-only package launch evidence. See $log"
        }
    }
    foreach ($client in 'http','webview') {
        foreach ($endpoint in @('public') + $local) {
            $pattern = if ($Ordinary -or $endpoint -eq 'public') { 'content' } else { 'failed-[A-Za-z0-9-]+|timeout' }
            if ($result -notmatch "(?m)sandbox-network-apk: $client $endpoint ($pattern)\s*$") {
                throw "$Mode $client $endpoint did not match qualification. See $log"
            }
        }
    }
    Write-Output "$Mode passed. Evidence $log"
}

$env:GESHER_SANDBOX_NETWORK = 'internet-only'
$env:GESHER_HWUI_RENDERER = 'skiacpu'
$env:GESHER_IP_GEOLOCATION = '0'
$env:GESHER_WEBVIEW_ARGS = '--disable-background-networking --disable-component-update --no-first-run --disable-sync --no-proxy-server'
RunProbe 'ordinary' $true
RunProbe 'contained' $false
RunProbe 'ordinary-after' $true
