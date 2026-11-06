# Actual two-APK cross-process ordered delivery. Only fresh synthetic profiles, all headless.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $RuntimeDir,
    [Parameter(Mandatory)][string] $OutputRoot,
    [string] $AppsDir,
    [switch] $ExpectBaselineFailure
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version 2
$RuntimeDir=(Resolve-Path -LiteralPath $RuntimeDir).Path
if(-not $AppsDir){$AppsDir=$RuntimeDir}
$AppsDir=(Resolve-Path -LiteralPath $AppsDir).Path
$run=Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('ordered-'+[guid]::NewGuid().ToString('N'))
$profile=Join-Path $run 'profile/Gesher'
$logs=Join-Path $run 'logs'
function A([string]$p){([IO.Path]::GetFullPath($p)-replace '\\','/')-replace '^[A-Za-z]:',''}
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
function Vm([string]$label,[string[]]$arguments){
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
    $start.EnvironmentVariables['GESHER_APP_WINDOW']='headless'
    $start.EnvironmentVariables['GESHER_IDLE_TIMEOUT']='3600'
    foreach($name in 'GESHER_BINDER','GESHER_BINDER_UID','GESHER_UID','GESHER_REQUIRE_BOOT_IMAGE'){$start.EnvironmentVariables.Remove($name)}
    if($script:device){$start.EnvironmentVariables['GESHER_BINDER']=$script:device}
    $process=New-Object Diagnostics.Process;$process.StartInfo=$start
    try{
        $null=$process.Start()
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(-not $process.WaitForExit(45000)){$process.Kill();throw "Synthetic ordered fixture VM timed out: $label"}
        $text=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult()
        [IO.File]::WriteAllText((Join-Path $run "$label.log"),$text,[Text.Encoding]::UTF8)
        if($process.ExitCode -ne 0){throw "Synthetic ordered fixture VM failed: $label"}
        return $text
    }finally{$process.Dispose()}
}
function Launcher([string]$label,[string[]]$arguments){Vm $label (@('com.gesher.app.Launcher')+$arguments)}
$started=$false
try{
    $null=Launcher 'install-target' @('install',(A (Join-Path $AppsDir 'orderedbroadcast-target.apk')))
    $started=$true
    foreach($line in [IO.File]::ReadAllLines((Join-Path $profile 'data/system/gesher-device'))){
        if($line -match '^device=(.+)$'){$script:device=$Matches[1].Trim()}
    }
    if(-not $script:device){throw 'No synthetic device namespace'}
    $null=Launcher 'install-source' @('install',(A (Join-Path $AppsDir 'orderedbroadcast-source.apk')))
    $null=Launcher 'send-ordered' @('launch','com.gesher.orderedbroadcast.source','--headless','--keep-task')
    $source=ReadLog (Join-Path $logs 'com.gesher.orderedbroadcast.source.log')
    $all=(@(Get-ChildItem -LiteralPath $logs -File -Filter '*.log'|ForEach-Object{ReadLog $_.FullName}))-join "`n"
    $pass=$source.Contains('ordered: PASS reason=final-once-after-gc finalCount=1') `
        -and $source.Contains('ordered: gc-complete receiverRetained=true') `
        -and $source.Contains('ordered: final received count=1 orderedHint=false code=303 propagated=true gc=true') `
        -and $all.Contains('ordered: dynamic received ordered=true initial=true') `
        -and $all.Contains('ordered: manifest received ordered=true propagated=true') `
        -and $all.Contains('ordered: dynamic async finished code=202')
    if($ExpectBaselineFailure){
        if($pass -or -not $all.Contains('ordered: dynamic received ordered=false') `
            -or -not $source.Contains('ordered: FAIL reason=timeout-final-callback finalCount=0')){
            throw "Expected baseline ordered-delivery gap was not reproduced: $run"
        }
        Write-Host 'PASS expected baseline: unordered target delivery and no final callback'
    }else{
        if(-not $pass -or $source.Contains('ordered: FAIL')){throw "Ordered broadcast integration failed: $run"}
        Write-Host 'PASS ordered broadcast: async sequential result propagation, separate manifest process, final callback once after GC'
    }
    $markers=@($all -split "`r?`n"|Where-Object{$_ -like '*ordered: *'})
    [IO.File]::WriteAllLines((Join-Path $run 'synthetic-markers.log'),$markers,[Text.Encoding]::UTF8)
    [pscustomobject]@{
        scope='Two synthetic ordinary APKs, fresh profile, all components headless; no network/accounts/real user data'
        expectedBaselineFailure=[bool]$ExpectBaselineFailure
        candidatePassed=$pass
        nativeSha256=(Get-FileHash -LiteralPath (Join-Path $RuntimeDir 'dalvikvm.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
        javaSha256=(Get-FileHash -LiteralPath (Join-Path $RuntimeDir 'gesher-app.jar') -Algorithm SHA256).Hash.ToLowerInvariant()
        sourceApkSha256=(Get-FileHash -LiteralPath (Join-Path $AppsDir 'orderedbroadcast-source.apk') -Algorithm SHA256).Hash.ToLowerInvariant()
        targetApkSha256=(Get-FileHash -LiteralPath (Join-Path $AppsDir 'orderedbroadcast-target.apk') -Algorithm SHA256).Hash.ToLowerInvariant()
        markerLog=Join-Path $run 'synthetic-markers.log'
    }|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $run 'validation.json') -Encoding UTF8
    Write-Host "Ordered broadcast evidence: $run"
}finally{
    if($started){$null=Launcher 'shutdown' @('shutdown')}
}
