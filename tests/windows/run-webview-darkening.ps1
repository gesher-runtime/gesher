# Real boundary calls and renderer pixels on an owned desktop that is never displayed.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $RuntimeDir,
    [Parameter(Mandatory)][string] $OutputRoot,
    [string] $Apk,
    [switch] $Worker
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version 2
function A([string]$path){([IO.Path]::GetFullPath($path)-replace '\\','/')-replace '^[A-Za-z]:',''}
function Q([string]$value){'"'+[regex]::Replace([regex]::Replace($value,'(\\*)"','$1$1\"'),'(\\+)$','$1$1')+'"'}
$RuntimeDir=(Resolve-Path -LiteralPath $RuntimeDir).Path
if(-not $Apk){$Apk=Join-Path $RuntimeDir 'webapp.apk'}
$Apk=(Resolve-Path -LiteralPath $Apk).Path
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
if($Worker){
    $profile=Join-Path $OutputRoot 'profile'
    foreach($part in 'system/framework','data','art','i18n/etc/icu'){
        New-Item -ItemType Directory -Path (Join-Path $profile $part) -Force|Out-Null
    }
    Copy-Item -LiteralPath (Join-Path $RuntimeDir 'framework-res.apk') -Destination (Join-Path $profile 'system/framework')
    Copy-Item -LiteralPath (Join-Path $RuntimeDir 'build.prop') -Destination (Join-Path $profile 'system')
    if(Test-Path -LiteralPath (Join-Path $RuntimeDir 'fonts/fonts.xml')){
        New-Item -ItemType Directory -Path (Join-Path $profile 'system/etc'),(Join-Path $profile 'system/fonts') -Force|Out-Null
        Copy-Item -LiteralPath (Join-Path $RuntimeDir 'fonts/fonts.xml') -Destination (Join-Path $profile 'system/etc')
        Get-ChildItem -LiteralPath (Join-Path $RuntimeDir 'fonts') -File -Filter '*.ttf'|Copy-Item -Destination (Join-Path $profile 'system/fonts')
    }
    $icu=@(Get-ChildItem -LiteralPath $RuntimeDir -File -Filter 'icudt*l.dat')
    if($icu.Count -ne 1){throw 'Exactly one ICU file required'}
    Copy-Item -LiteralPath $icu[0].FullName -Destination (Join-Path $profile 'i18n/etc/icu')
    $env:ANDROID_ROOT=A (Join-Path $profile 'system')
    $env:ANDROID_DATA=A (Join-Path $profile 'data')
    $env:ANDROID_ART_ROOT=A (Join-Path $profile 'art')
    $env:ANDROID_I18N_ROOT=A (Join-Path $profile 'i18n')
    $env:ANDROID_TZDATA_ROOT=A (Join-Path $RuntimeDir 'tzdata')
    $env:LOCALAPPDATA=Join-Path $profile 'local'
    $env:APPDATA=Join-Path $profile 'roaming'
    $env:GESHER_SHORTCUTS='0'
    $env:GESHER_IP_GEOLOCATION='0'
    $env:GESHER_REQUIRE_BOOT_IMAGE='1'
    $env:GESHER_LOCALES='en-US'
    foreach($name in 'GESHER_BINDER','GESHER_BINDER_UID','GESHER_UID','GESHER_APP_WINDOW','GESHER_WINDOW_MODE','GESHER_WEBVIEW_ARGS'){
        Remove-Item "Env:$name" -ErrorAction SilentlyContinue
    }
    $boot=(@('core-oj','core-libart','core-icu4j','framework','gesher-app')|ForEach-Object{A (Join-Path $RuntimeDir "$_.jar")})-join ':'
    $arguments=@("-Xbootclasspath:$boot","-Ximage:$(A (Join-Path $RuntimeDir 'framework/boot.art'))",'-Xint','-Xusejit:false',
        'com.gesher.app.Main','--window','--display','600x900@160','--data-dir',(A (Join-Path $profile 'app-data')),
        '--activity','com.gesher.webapp.DarkeningActivity',(A $Apk))
    $start=New-Object Diagnostics.ProcessStartInfo
    $start.FileName=Join-Path $RuntimeDir 'dalvikvm.exe'
    $start.WorkingDirectory=$RuntimeDir
    $start.Arguments=($arguments|ForEach-Object{Q $_})-join ' '
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    $process=New-Object Diagnostics.Process;$process.StartInfo=$start
    try{
        $null=$process.Start()
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(-not $process.WaitForExit(90000)){$process.Kill();throw 'Darkening fixture VM timed out'}
        $text=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult()
        [IO.File]::WriteAllText((Join-Path $OutputRoot 'fixture.log'),$text,[Text.Encoding]::UTF8)
        if($process.ExitCode -ne 0 -or $text.Contains('webdark: FAIL') -or -not $text.Contains('webdark: ALL PASSED ')){
            throw 'Darkening fixture failed; see fixture.log'
        }
        if(-not $text.Contains('gesher: boot image loaded: 5 spaces (boot.art boot-core-libart.art boot-core-icu4j.art boot-framework.art boot-gesher-app.art)') -or $text.Contains('imageless')){
            throw 'Matching five-space boot image was not used'
        }
    }finally{$process.Dispose()}
    exit 0
}
$run=Join-Path $OutputRoot ('webview-darkening-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run -Force|Out-Null
$images=@(Get-ChildItem -LiteralPath (Join-Path $RuntimeDir 'framework/x86_64') -File|Where-Object{$_.Name-match '^boot(?:-core-libart|-core-icu4j|-framework|-gesher-app)?\.(art|oat|vdex)$'})
if($images.Count -ne 15){throw 'All15matching boot image files are required'}
if(-not ('PrivateDesktopProcess' -as [type])){Add-Type -Path "$PSScriptRoot/helpers/PrivateDesktopProcess.cs"}
$arguments=@('powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',$PSCommandPath,
    '-RuntimeDir',$RuntimeDir,'-OutputRoot',$run,'-Apk',$Apk,'-Worker')
$command=($arguments|ForEach-Object{Q $_})-join ' '
[IO.File]::WriteAllText((Join-Path $run 'command.txt'),$command,[Text.Encoding]::UTF8)
$code=[PrivateDesktopProcess]::Run($command,$RuntimeDir,110000)
if($code -ne 0){throw "Private desktop darkening worker failed exit=$code; see $run"}
$paths=@((Join-Path $RuntimeDir 'dalvikvm.exe'),(Join-Path $RuntimeDir 'gesher-app.jar'),
    (Join-Path $RuntimeDir 'framework.jar'),(Join-Path $RuntimeDir 'libgesher_webview_bridge.so'),$Apk)+@($images.FullName)
$hashes=@($paths|ForEach-Object{[pscustomobject]@{path=$_;sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLowerInvariant()}})
[pscustomobject]@{scope='Synthetic boundary signatures and actual three-view renderer pixels; owned never-switched desktop; no accounts/network/user data';
    passed=$true;exit=$code;artifacts=$hashes}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $run 'validation.json') -Encoding UTF8
Write-Host "PASS WebView darkening boundary and renderer; evidence $run"
