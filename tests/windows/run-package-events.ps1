# Actual package publication/cache events with ordinary APKs and fresh headless profiles.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $RuntimeDir,
    [Parameter(Mandatory)][string] $OutputRoot,
    [string] $AppsDir,
    [switch] $ExpectBaselineFailure,
    [switch] $RequireBootImages
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version 2
$RuntimeDir=(Resolve-Path -LiteralPath $RuntimeDir).Path
if(-not $AppsDir){$AppsDir=$RuntimeDir}
$AppsDir=(Resolve-Path -LiteralPath $AppsDir).Path
$run=Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('package-events-'+[guid]::NewGuid().ToString('N'))
$profile=Join-Path $run 'profile/Gesher'
$logs=Join-Path $run 'logs'
function A([string]$p){
    $value=[IO.Path]::GetFullPath($p)-replace '\\','/'
    if($value -notmatch '^([A-Za-z]):(.*)$'){throw 'A local Windows path is required'}
    if($Matches[1] -ieq $RuntimeDir.Substring(0,1)){return $Matches[2]}
    return "/mnt/$($Matches[1].ToLowerInvariant())$($Matches[2])"
}
function Q([string]$p){'"'+[regex]::Replace([regex]::Replace($p,'(\\*)"','$1$1\"'),'(\\+)$','$1$1')+'"'}
function ReadLog([string]$p){
    if(-not(Test-Path -LiteralPath $p)){return ''}
    $stream=[IO.FileStream]::new($p,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    $reader=[IO.StreamReader]::new($stream,[Text.Encoding]::UTF8,$true)
    try{$reader.ReadToEnd()}finally{$reader.Dispose()}
}
foreach($part in 'system/framework','system/etc','system/fonts','data','art','i18n/etc/icu'){
    New-Item -ItemType Directory -Path (Join-Path $profile $part) -Force|Out-Null
}
New-Item -ItemType Directory -Path $logs -Force|Out-Null
$icu=@(Get-ChildItem -LiteralPath $RuntimeDir -File -Filter 'icudt*l.dat')
if($icu.Count -ne 1){throw 'Exactly one ICU data file required'}
Copy-Item -LiteralPath $icu[0].FullName -Destination (Join-Path $profile 'i18n/etc/icu')
Copy-Item -LiteralPath (Join-Path $RuntimeDir 'framework-res.apk') -Destination (Join-Path $profile 'system/framework')
Copy-Item -LiteralPath (Join-Path $RuntimeDir 'build.prop') -Destination (Join-Path $profile 'system')
if(Test-Path -LiteralPath (Join-Path $RuntimeDir 'fonts/fonts.xml')){
    Copy-Item -LiteralPath (Join-Path $RuntimeDir 'fonts/fonts.xml') -Destination (Join-Path $profile 'system/etc')
    Get-ChildItem -LiteralPath (Join-Path $RuntimeDir 'fonts') -File -Filter '*.ttf'|Copy-Item -Destination (Join-Path $profile 'system/fonts')
}
$bcp=(@('core-oj.jar','core-libart.jar','core-icu4j.jar','framework.jar','gesher-app.jar')|ForEach-Object{A (Join-Path $RuntimeDir $_)})-join ':'
$script:vm=@("-Xbootclasspath:$bcp","-Ximage:$(A (Join-Path $RuntimeDir 'framework/boot.art'))",'-Xint','-Xusejit:false')
$script:device=$null
function Vm([string]$label,[string[]]$arguments,[int]$ExpectedExit=0){
    $start=New-Object Diagnostics.ProcessStartInfo
    $start.FileName=Join-Path $RuntimeDir 'dalvikvm.exe'
    $start.WorkingDirectory=$RuntimeDir
    $start.Arguments=(($script:vm+$arguments|ForEach-Object{Q $_})-join ' ')
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    $start.EnvironmentVariables['ANDROID_ROOT']=A (Join-Path $profile 'system')
    $start.EnvironmentVariables['ANDROID_DATA']=A (Join-Path $profile 'data')
    $start.EnvironmentVariables['ANDROID_ART_ROOT']=A (Join-Path $profile 'art')
    $start.EnvironmentVariables['ANDROID_I18N_ROOT']=A (Join-Path $profile 'i18n')
    $start.EnvironmentVariables['ANDROID_TZDATA_ROOT']=A (Join-Path $RuntimeDir 'tzdata')
    $start.EnvironmentVariables['LOCALAPPDATA']=Join-Path $run 'profile'
    $start.EnvironmentVariables['APPDATA']=Join-Path $run 'roaming'
    $start.EnvironmentVariables['GESHER_LOGS']=A $logs
    $start.EnvironmentVariables['GESHER_SHORTCUTS']='0'
    $start.EnvironmentVariables['GESHER_IP_GEOLOCATION']='0'
    $start.EnvironmentVariables['GESHER_MICROG_SPOOF']='1'
    $start.EnvironmentVariables['GESHER_APP_WINDOW']='headless'
    $start.EnvironmentVariables['GESHER_IDLE_TIMEOUT']='3600'
    foreach($name in 'GESHER_BINDER','GESHER_BINDER_UID','GESHER_UID','GESHER_REQUIRE_BOOT_IMAGE'){$start.EnvironmentVariables.Remove($name)}
    if($RequireBootImages){$start.EnvironmentVariables['GESHER_REQUIRE_BOOT_IMAGE']='1'}
    if($arguments -contains 'gesher.fixture.PackageEventsControl'){$start.EnvironmentVariables['GESHER_UID']='2000';$start.EnvironmentVariables['GESHER_BINDER_UID']='2000'}
    if($script:device){$start.EnvironmentVariables['GESHER_BINDER']=$script:device}
    $process=New-Object Diagnostics.Process;$process.StartInfo=$start
    try{
        $null=$process.Start()
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        $timedOut=-not $process.WaitForExit(45000)
        if($timedOut){$process.Kill();$null=$process.WaitForExit(5000)}
        $captureComplete=[Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($stdout,$stderr),5000)
        $text=if($captureComplete){$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult()}else{'capture incomplete after bounded timeout'}
        [IO.File]::WriteAllText((Join-Path $run "$label.log"),$text,[Text.Encoding]::UTF8)
        if($timedOut -or -not $captureComplete){throw "Synthetic package-event VM timed out: $label"}
        if($process.ExitCode -ne $ExpectedExit){throw "Synthetic package-event VM failed: $label"}
        if($RequireBootImages -and (-not $text.Contains('gesher: boot image loaded: 5 spaces (boot.art boot-core-libart.art boot-core-icu4j.art boot-framework.art boot-gesher-app.art)') -or $text.Contains('imageless'))){throw "Missing complete boot images: $label"}
        return $text
    }finally{$process.Dispose()}
}
function Launcher([string]$label,[string[]]$arguments,[int]$ExpectedExit=0){Vm $label (@('com.gesher.app.Launcher')+$arguments) $ExpectedExit}

function Control([string]$command){Vm ('control-'+$command) @('-cp',(A (Join-Path $AppsDir 'packageevents-control.dex')),'gesher.fixture.PackageEventsControl',$command)}
function ObserverLog{ReadLog (Join-Path $logs 'com.gesher.packageevents.observer.log')}
function WaitMarker([string]$marker,[int]$Timeout=15000){
    $deadline=[DateTime]::UtcNow.AddMilliseconds($Timeout)
    do{$text=ObserverLog;if($text.Contains('packageevents: FAIL')){throw "Observer reported failure: $run"};if($text.Contains($marker)){return};Start-Sleep -Milliseconds 50}while([DateTime]::UtcNow -lt $deadline)
    throw "Observer marker timed out: $marker"
}
$started=$false
$success=$false
try{
    $started=$true
    $null=Launcher 'install-observer' @('install',(A (Join-Path $AppsDir 'packageevents-observer.apk')))
    foreach($line in [IO.File]::ReadAllLines((Join-Path $profile 'data/system/gesher-device'))){if($line -match '^device=(.+)$'){$script:device=$Matches[1].Trim()}}
    if(-not $script:device){throw 'No synthetic device namespace'}
    $null=Launcher 'start-observer' @('launch','com.gesher.packageevents.observer','--activity','.BootstrapActivity','--headless','--keep-task')
    WaitMarker 'READY absent-cache-warmed=true'
    WaitMarker 'spoof-blocked=true'
    $null=Launcher 'install-target-v1' @('install',(A (Join-Path $AppsDir 'packageevents-target-v1.apk')))
    $probe=Control 'probe1';if(-not $probe.Contains('target-installed version=1')){throw 'First target not installed'}
    if($ExpectBaselineFailure){
        $null=Control 'baseline-warm';WaitMarker 'BASELINE_WARM version=1 count=0'
        $null=Launcher 'install-target-v2' @('install',(A (Join-Path $AppsDir 'packageevents-target-v2.apk')))
        $probe=Control 'probe2';if(-not $probe.Contains('target-installed version=2')){throw 'Updated target not installed'}
        $null=Control 'baseline-finish';WaitMarker 'BASELINE EXPECTED no-events=true'
        Write-Host 'PASS expected baseline: real installed version2, zero lifecycle events; observed cache version recorded'
    }else{
        WaitMarker 'ADDED version=1 replacing=false uid-match=true count=1'
        $null=Launcher 'install-unchanged-v1' @('install',(A (Join-Path $AppsDir 'packageevents-target-v1.apk')))
        $invalid=Join-Path $run 'invalid.apk';[IO.File]::WriteAllBytes($invalid,[byte[]](1,2,3))
        $null=Launcher 'install-invalid' @('install',(A $invalid)) 1
        $null=Control 'quiet1';WaitMarker 'QUIET command=quiet1 count=1 version=1'
        $null=Launcher 'install-target-v2' @('install',(A (Join-Path $AppsDir 'packageevents-target-v2.apk')))
        $probe=Control 'probe2';if(-not $probe.Contains('target-installed version=2')){throw 'Updated target not installed'}
        WaitMarker 'ADDED version=2 replacing=true uid-match=true count=2'
        WaitMarker 'REPLACED version=2 uid-match=true count=3'
        $targetLog=Join-Path $logs 'com.gesher.packageevents.target.log';$deadline=[DateTime]::UtcNow.AddSeconds(15)
        while(-not (ReadLog $targetLog).Contains('packageevents-target: MY_REPLACED version=2') -and [DateTime]::UtcNow -lt $deadline){Start-Sleep -Milliseconds 50}
        if(-not (ReadLog $targetLog).Contains('packageevents-target: MY_REPLACED version=2')){throw 'Targeted MY_PACKAGE_REPLACED not delivered'}
        $null=Launcher 'install-unchanged-v2' @('install',(A (Join-Path $AppsDir 'packageevents-target-v2.apk')))
        $null=Launcher 'install-invalid-again' @('install',(A $invalid)) 1
        $null=Control 'finish';WaitMarker 'PASS fresh-update-unchanged-invalid-cache-and-spoof'
        Write-Host 'PASS actual package events: fresh/update order, real UID/cache, targeted replacement, unchanged/invalid quiet, ordinary spoof denial'
    }
    if($RequireBootImages){
        foreach($file in Get-ChildItem -LiteralPath $logs -File -Filter '*.log'|Where-Object{$_.Name -eq 'system_server.log' -or $_.Name.StartsWith('com.gesher.packageevents.')}){
            $text=ReadLog $file.FullName
            if(-not $text.Contains('gesher: boot image loaded: 5 spaces (boot.art boot-core-libart.art boot-core-icu4j.art boot-framework.art boot-gesher-app.art)') -or $text.Contains('imageless')){throw "Child did not load complete images: $($file.Name)"}
        }
    }
    $all=(@(Get-ChildItem -LiteralPath $logs -File -Filter '*.log'|ForEach-Object{ReadLog $_.FullName}))-join "`n"
    $markers=@($all -split "`r?`n"|Where-Object{$_ -match 'packageevents(-target)?: '})
    [IO.File]::WriteAllLines((Join-Path $run 'synthetic-markers.log'),$markers,[Text.Encoding]::UTF8)
    $hashes=@{};foreach($file in 'packageevents-observer.apk','packageevents-target-v1.apk','packageevents-target-v2.apk','packageevents-control.dex'){$hashes[$file]=(Get-FileHash -LiteralPath (Join-Path $AppsDir $file) -Algorithm SHA256).Hash.ToLowerInvariant()}
    [pscustomobject]@{scope='Ordinary synthetic observer/target APKs and trusted host installs in a fresh headless profile; no accounts/network/real user data';expectedBaselineFailure=[bool]$ExpectBaselineFailure;passed=$true;requireCompleteBootImages=[bool]$RequireBootImages;apkHashes=$hashes;javaSha256=(Get-FileHash -LiteralPath (Join-Path $RuntimeDir 'gesher-app.jar') -Algorithm SHA256).Hash.ToLowerInvariant();servicesSha256=(Get-FileHash -LiteralPath (Join-Path $RuntimeDir 'services-tests.dex') -Algorithm SHA256).Hash.ToLowerInvariant()}|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $run 'validation.json') -Encoding UTF8
    Write-Host "Package-event evidence: $run"
    $success=$true
}finally{
    if($started){try{$null=Launcher 'shutdown' @('shutdown')}catch{
        [IO.File]::WriteAllText((Join-Path $run 'cleanup-error.log'),$_.Exception.ToString())
        if($success){throw}
        Write-Warning 'Fixture cleanup failed; original failure retained.'
    }}
}
