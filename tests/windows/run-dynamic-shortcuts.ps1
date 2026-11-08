# Copyright (C) 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
# Publish a real shortcut and launch it through the host CLI on an isolated profile.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $RuntimeDir,
    [Parameter(Mandatory)][string] $OutputRoot,
    [string] $Apk,
    [switch] $RequireBootImages
)
$ErrorActionPreference='Stop'
# A pwsh parent may omit this Windows PowerShell host's script modules.
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
Set-StrictMode -Version 2
$RuntimeDir=(Resolve-Path -LiteralPath $RuntimeDir).Path
if(-not $Apk){$Apk=Join-Path $RuntimeDir 'lifecycle.apk'}
$Apk=(Resolve-Path -LiteralPath $Apk).Path
$run=Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('dynamic-shortcut-'+[guid]::NewGuid().ToString('N'))
$profile=Join-Path $run 'profile/Gesher'
$logs=Join-Path $run 'logs'
$script:device=$null
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
$vm=@("-Xbootclasspath:$bcp","-Ximage:$(A (Join-Path $RuntimeDir 'framework/boot.art'))",'-Xint','-Xusejit:false','com.gesher.app.Launcher')
$expectedBoot='gesher: boot image loaded: 5 spaces (boot.art boot-core-libart.art boot-core-icu4j.art boot-framework.art boot-gesher-app.art)'
function Launcher([string]$label,[string[]]$arguments,[int]$timeout=30000){
    $start=New-Object Diagnostics.ProcessStartInfo
    $start.FileName=Join-Path $RuntimeDir 'dalvikvm.exe';$start.WorkingDirectory=$RuntimeDir
    $start.Arguments=(($vm+$arguments|ForEach-Object{Q $_})-join ' ')
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
    # This OS-exit regression asserts the English CLI diagnostic; UI selection has
    # separate bilingual tests. Do not inherit the host's display language here.
    $start.EnvironmentVariables['GESHER_LOCALES']='en-US'
    $start.EnvironmentVariables['GESHER_SHORTCUTS']='0';$start.EnvironmentVariables['GESHER_IP_GEOLOCATION']='0'
    $start.EnvironmentVariables['GESHER_APP_WINDOW']='headless';$start.EnvironmentVariables['GESHER_IDLE_TIMEOUT']='3600'
    foreach($name in 'GESHER_BINDER','GESHER_BINDER_UID','GESHER_UID','GESHER_REQUIRE_BOOT_IMAGE'){$start.EnvironmentVariables.Remove($name)}
    if($script:device){$start.EnvironmentVariables['GESHER_BINDER']=$script:device}
    if($RequireBootImages){$start.EnvironmentVariables['GESHER_REQUIRE_BOOT_IMAGE']='1'}
    if($label -eq 'publish'){$start.EnvironmentVariables['GESHER_BINDER_UID']='2000';$start.EnvironmentVariables['GESHER_UID']='2000'}
    $process=New-Object Diagnostics.Process;$process.StartInfo=$start
    try{
        $null=$process.Start()
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        $finished=$process.WaitForExit($timeout)
        if(-not $finished){$process.Kill();$null=$process.WaitForExit(5000)}
        $text=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult()
        [IO.File]::WriteAllText((Join-Path $run "$label.log"),$text,[Text.Encoding]::UTF8)
        if($RequireBootImages -and -not $text.Contains($expectedBoot)){throw "CLI did not load complete images: $label"}
        [pscustomobject]@{finished=$finished;exitCode=$process.ExitCode;text=$text}
    }finally{$process.Dispose()}
}

$started=$false
try {
    $install=Launcher 'install' @('install',(A $Apk))
    if(-not $install.finished -or $install.exitCode -ne 0){throw 'Shortcut CLI fixture install failed'}
    $started=$true
    foreach($line in [IO.File]::ReadAllLines((Join-Path $profile 'data/system/gesher-device'))){if($line -match '^device=(.+)$'){$script:device=$Matches[1].Trim()}}
    $normalVm=$vm
    $script:vm=$normalVm[0..3]+@('-cp',(A (Join-Path $RuntimeDir 'services-tests.dex')),'gesher.services.DynamicShortcutTest')
    $publish=Launcher 'publish' @('--publish','com.gesher.lifecycle')
    $script:vm=$normalVm
    if(-not $publish.finished -or $publish.exitCode -ne 0){throw 'Shortcut CLI publication failed'}
    $key=[regex]::Match($publish.text,'SHORTCUT_KEY=([0-9a-f]{64})').Groups[1].Value
    if(-not $key){throw 'No opaque shortcut key returned'}
    $launch=Launcher 'shortcut-launch' @('launch','com.gesher.lifecycle','--shortcut',$key,'--headless')
    if(-not $launch.finished -or $launch.exitCode -ne 0 -or -not $launch.text.Contains('LifecycleActivity onResume')){throw 'Actual publisher-bound shortcut launch failed'}
    Write-Output "PASS real cross-process shortcut publication, opaque CLI arguments, app resume and launch exit receipt; evidence $run"
} finally {
    if($normalVm){$script:vm=$normalVm}
    if($started){$null=Launcher 'shutdown' @('shutdown')}
}
