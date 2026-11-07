# Synthetic service-first Activity promotion on an owned, never-displayed desktop.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $RuntimeDir,
    [Parameter(Mandatory)][string] $FixtureDir,
    [Parameter(Mandatory)][string] $OutputRoot,
    [string] $Controller,
    [switch] $Worker,
    [ValidateSet('positive','explicit-headless')][string] $Mode = 'positive'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
function A([string] $p) { ([IO.Path]::GetFullPath($p) -replace '\\','/') -replace '^[A-Za-z]:','' }
function Q([string] $p) {
    '"' + [regex]::Replace([regex]::Replace($p, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}
function LogText([string] $p) {
    if (-not (Test-Path -LiteralPath $p)) { return '' }
    $file = [IO.FileStream]::new($p, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    $reader = [IO.StreamReader]::new($file, [Text.Encoding]::UTF8, $true)
    try { $reader.ReadToEnd() } finally { $reader.Dispose() }
}
$RuntimeDir = (Resolve-Path -LiteralPath $RuntimeDir).Path
$FixtureDir = (Resolve-Path -LiteralPath $FixtureDir).Path
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if ($Worker) {
    $boot = (@('core-oj.jar','core-libart.jar','core-icu4j.jar','framework.jar','gesher-app.jar') | ForEach-Object { A (Join-Path $RuntimeDir $_) }) -join ':'
    $base = @("-Xbootclasspath:$boot", "-Ximage:$(A (Join-Path $RuntimeDir 'framework/boot.art'))", '-Xint','-Xusejit:false')
    $app = A (Join-Path $RuntimeDir 'gesher-app.jar')
    $control = A (Join-Path $FixtureDir 'windowpromotion-control.dex')
    $logs = Join-Path $OutputRoot 'logs'
    $profile = Join-Path $OutputRoot 'profile/Gesher'
    function Vm([string] $label, [string[]] $arguments) {
        $start = New-Object Diagnostics.ProcessStartInfo
        $start.FileName = Join-Path $RuntimeDir 'dalvikvm.exe'
        $start.WorkingDirectory = $RuntimeDir
        $start.Arguments = (($base + $arguments | ForEach-Object { Q $_ }) -join ' ')
        $start.UseShellExecute = $false; $start.CreateNoWindow = $true
        $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
        $process = New-Object Diagnostics.Process; $process.StartInfo = $start
        try {
            $null = $process.Start()
            $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
            if (-not $process.WaitForExit(60000)) { $process.Kill(); throw "Fixture VM timed out: $label" }
            $text = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
            [IO.File]::WriteAllText((Join-Path $OutputRoot "$label.log"), $text, [Text.Encoding]::UTF8)
            if ($process.ExitCode -ne 0) { throw "Fixture VM failed: $label; see its synthetic log" }
            return $text
        } finally { $process.Dispose() }
    }
    function Launch([string] $label, [string[]] $arguments) { Vm $label (@('-cp',$app,'com.gesher.app.Launcher') + $arguments) }
    function Control([string] $command) { Vm $command @('-cp',"${control}:$app",'WindowPromotionControl',$command) }
    function WaitMarker([string] $path, [string] $marker) {
        $deadline = [DateTime]::UtcNow.AddSeconds(30)
        do {
            $text = LogText $path
            if ($text.Contains($marker)) { return $text }
            Start-Sleep -Milliseconds 50
        } while ([DateTime]::UtcNow -lt $deadline)
        throw "Synthetic fixture missing marker: $marker"
    }
    $started = $false
    try {
        $null = Launch 'install-target' @('install',(A (Join-Path $FixtureDir 'windowpromotion-target.apk')))
        $started = $true
        $state = @{}
        foreach ($line in [IO.File]::ReadAllLines((Join-Path $profile 'data/system/gesher-device'))) {
            if ($line -notmatch '^\s*[#!]' -and $line -match '=') { $key,$value=$line -split '=',2; $state[$key.Trim()]=$value.Trim() }
        }
        if (-not $state.ContainsKey('device')) { throw 'No synthetic device namespace' }
        $env:GESHER_BINDER = $state['device']
        $null = Launch 'install-source' @('install',(A (Join-Path $FixtureDir 'windowpromotion-source.apk')))
        if ($Mode -eq 'explicit-headless') { $null = Control 'explicit-headless' }
        $null = Launch 'bootstrap-service' @('launch','com.gesher.windowpromotion.source','--activity','.BootstrapActivity','--headless')
        $targetLog = Join-Path $logs 'com.gesher.windowpromotion.target.log'
        $text = WaitMarker $targetLog 'promotion: service-created pid='
        $match = [regex]::Match($text,'promotion: service-created pid=([0-9]+)')
        if (-not $match.Success) { throw 'No service process ID' }
        $originalPid = [int]$match.Groups[1].Value
        $null = Launch 'delegate' @('launch','com.gesher.windowpromotion.source','--window')
        $text = WaitMarker $targetLog "promotion: target-created pid=$originalPid service=true"
        [IO.File]::WriteAllText((Join-Path $OutputRoot 'state.txt'), "CHECK $originalPid", [Text.Encoding]::ASCII)
        if ($Mode -eq 'positive') { $text = WaitMarker $targetLog "promotion: frame-pass pid=$originalPid service=true" }
        if ($text.Contains('promotion: service-destroyed')) { throw 'Original service was destroyed' }
        if ([regex]::Matches($text,'promotion: service-created pid=').Count -ne 1) { throw 'Target process/service restarted' }
        $deadline = [DateTime]::UtcNow.AddSeconds(20)
        while (-not (Test-Path -LiteralPath (Join-Path $OutputRoot 'controller-pass.txt'))) {
            if ([DateTime]::UtcNow -ge $deadline) { throw 'Private-desktop window controller timed out' }
            Start-Sleep -Milliseconds 25
        }
        $text = LogText $targetLog
        if ([regex]::Matches($text,"promotion: heartbeat pid=$originalPid\b").Count -lt 2) { throw 'Original service heartbeat not retained' }
        if ($text.Contains('promotion: service-destroyed')) { throw 'Original service died before window proof' }
        if ($Mode -eq 'positive') {
            $null = Vm 'begin-background-phase' @('-cp',$app,'com.gesher.server.RuntimeCommands','am','broadcast','-n',
                'com.gesher.windowpromotion.target/com.gesher.windowpromotion.target.ControlReceiver')
            $null = WaitMarker $targetLog "promotion: first-activity-destroyed pid=$originalPid"
            $null = WaitMarker $targetLog "promotion: unapproved-created pid=$originalPid service=true"
            $text = WaitMarker $targetLog "promotion: unapproved-frame-pass pid=$originalPid service=true"
            if ($text.Contains('promotion: service-destroyed') -or [regex]::Matches($text,'promotion: service-created pid=').Count -ne 1) {
                throw 'Service changed during unapproved background phase'
            }
            [IO.File]::WriteAllText((Join-Path $OutputRoot 'state.txt'), "BACKGROUND $originalPid", [Text.Encoding]::ASCII)
            $null = WaitMarker (Join-Path $OutputRoot 'controller-pass.txt') 'BACKGROUND_PASS'
            Write-Host 'WindowPromotion unapproved background root rendered but remained offscreen'
        }
        Write-Host "WindowPromotion synthetic worker passed: $Mode samePid=true serviceAlive=true"
    } finally {
        if ($started) { $null = Launch 'shutdown' @('shutdown') }
    }
    exit 0
}
if (-not $Controller) { $Controller = Join-Path $FixtureDir 'window_promotion_test.exe' }
$Controller = (Resolve-Path -LiteralPath $Controller).Path
$run = Join-Path $OutputRoot ('promotion-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run -Force | Out-Null
$results = @()
foreach ($modeName in 'positive','explicit-headless') {
    $dir = Join-Path $run $modeName
    $profile = Join-Path $dir 'profile/Gesher'
    foreach ($part in 'system/framework','system/etc','data','art','i18n/etc/icu') { New-Item -ItemType Directory -Path (Join-Path $profile $part) -Force | Out-Null }
    New-Item -ItemType Directory -Path (Join-Path $dir 'logs') -Force | Out-Null
    $icu = @(Get-ChildItem -LiteralPath $RuntimeDir -Filter 'icudt*l.dat' -File)
    if ($icu.Count -ne 1) { throw 'Exactly one ICU file required' }
    Copy-Item -LiteralPath $icu[0].FullName -Destination (Join-Path $profile 'i18n/etc/icu')
    Copy-Item -LiteralPath (Join-Path $RuntimeDir 'framework-res.apk') -Destination (Join-Path $profile 'system/framework')
    Copy-Item -LiteralPath (Join-Path $RuntimeDir 'build.prop') -Destination (Join-Path $profile 'system')
    if (Test-Path -LiteralPath (Join-Path $RuntimeDir 'fonts/fonts.xml')) {
        New-Item -ItemType Directory -Path (Join-Path $profile 'system/fonts') -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $RuntimeDir 'fonts/fonts.xml') -Destination (Join-Path $profile 'system/etc')
        Get-ChildItem -LiteralPath (Join-Path $RuntimeDir 'fonts') -Filter '*.ttf' -File | Copy-Item -Destination (Join-Path $profile 'system/fonts')
    }
    $workerArgs = @('powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',$PSCommandPath,
        '-RuntimeDir',$RuntimeDir,'-FixtureDir',$FixtureDir,'-OutputRoot',$dir,'-Worker','-Mode',$modeName)
    $commandFile = Join-Path $dir 'command.txt'
    [IO.File]::WriteAllText($commandFile, (($workerArgs | ForEach-Object { Q $_ }) -join ' '), [Text.Encoding]::Unicode)
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $Controller
    $start.Arguments = (@($commandFile,$RuntimeDir,(Join-Path $dir 'state.txt'),(Join-Path $dir 'controller-pass.txt'),$modeName) | ForEach-Object { Q $_ }) -join ' '
    $start.WorkingDirectory = $RuntimeDir
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    $start.EnvironmentVariables['ANDROID_ROOT']=A (Join-Path $profile 'system')
    $start.EnvironmentVariables['ANDROID_DATA']=A (Join-Path $profile 'data')
    $start.EnvironmentVariables['ANDROID_ART_ROOT']=A (Join-Path $profile 'art')
    $start.EnvironmentVariables['ANDROID_I18N_ROOT']=A (Join-Path $profile 'i18n')
    $start.EnvironmentVariables['ANDROID_TZDATA_ROOT']=A (Join-Path $RuntimeDir 'tzdata')
    $start.EnvironmentVariables['LOCALAPPDATA']=Join-Path $dir 'profile'
    $start.EnvironmentVariables['APPDATA']=Join-Path $dir 'roaming'
    $start.EnvironmentVariables['GESHER_LOGS']=A (Join-Path $dir 'logs')
    $start.EnvironmentVariables['GESHER_SHORTCUTS']='0'
    $start.EnvironmentVariables['GESHER_IP_GEOLOCATION']='0'
    $start.EnvironmentVariables['GESHER_APP_WINDOW']='headless'
    $start.EnvironmentVariables['GESHER_IDLE_TIMEOUT']='3600'
    foreach ($name in 'GESHER_BINDER','GESHER_BINDER_UID','GESHER_UID','GESHER_WINDOW_MODE','GESHER_REQUIRE_BOOT_IMAGE') { $start.EnvironmentVariables.Remove($name) }
    $process = New-Object Diagnostics.Process; $process.StartInfo=$start
    try {
        $null=$process.Start()
        $stdout=$process.StandardOutput.ReadToEndAsync(); $stderr=$process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(160000)) { $process.Kill(); throw 'Synthetic controller timed out' }
        $text=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult()
        [IO.File]::WriteAllText((Join-Path $dir 'controller.log'),$text,[Text.Encoding]::UTF8)
        if ($process.ExitCode -ne 0 -or -not $text.Contains('window_promotion_test all passed:')) { throw "Window promotion failed: $modeName; see $dir" }
        $results += [pscustomobject]@{mode=$modeName;passed=$true;directory=$dir}
        Write-Host "PASS  window promotion $modeName"
    } finally { $process.Dispose() }
}
[pscustomobject]@{scope='Two synthetic APKs; fresh profiles; private never-switched desktops; same service PID, presented pixels, window bounds and explicit-headless control';results=$results} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $run 'validation.json') -Encoding UTF8
Write-Host 'WindowPromotion all 2 modes passed'
