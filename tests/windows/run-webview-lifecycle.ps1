# Ordinary headless APKs, fresh process state, no network/accounts/browser page creation.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $RuntimeDir,
    [Parameter(Mandatory)][string] $OutputRoot,
    [string] $Apk,
    [ValidateSet('disable-before-use','disable-after-use','concurrent-use-disable','suffix-isolation','invalid-suffix')]
    [string] $OnlyCase,
    [switch] $ExpectBaselineFailure
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version 2
$RuntimeDir=(Resolve-Path -LiteralPath $RuntimeDir).Path
if(-not $Apk){$Apk=Join-Path $RuntimeDir 'webviewlifecycle.apk'}
$Apk=(Resolve-Path -LiteralPath $Apk).Path
if($ExpectBaselineFailure -and $OnlyCase -and $OnlyCase -ne 'disable-before-use'){throw 'Baseline control is the startup-disable case only'}
$run=Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('webview-lifecycle-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run -Force|Out-Null
function A([string]$p){([IO.Path]::GetFullPath($p)-replace '\\','/')-replace '^[A-Za-z]:',''}
function Q([string]$p){'"'+[regex]::Replace([regex]::Replace($p,'(\\*)"','$1$1\"'),'(\\+)$','$1$1')+'"'}
function ReadLog([string]$p){
    if(-not(Test-Path -LiteralPath $p)){return ''}
    $stream=[IO.FileStream]::new($p,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    $reader=[IO.StreamReader]::new($stream,[Text.Encoding]::UTF8,$true)
    try{$reader.ReadToEnd()}finally{$reader.Dispose()}
}
$bcp=(@('core-oj.jar','core-libart.jar','core-icu4j.jar','framework.jar','gesher-app.jar')|ForEach-Object{A (Join-Path $RuntimeDir $_)})-join ':'
$script:vm=@("-Xbootclasspath:$bcp","-Ximage:$(A (Join-Path $RuntimeDir 'framework/boot.art'))",'-Xint','-Xusejit:false')
$script:device=$null
$script:profile=$null
$script:logs=$null
$script:caseDir=$null
function Vm([string]$label,[string[]]$arguments){
    $start=New-Object Diagnostics.ProcessStartInfo
    $start.FileName=Join-Path $RuntimeDir 'dalvikvm.exe'
    $start.WorkingDirectory=$RuntimeDir
    $start.Arguments=(($script:vm+$arguments|ForEach-Object{Q $_})-join ' ')
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    $start.EnvironmentVariables['ANDROID_ROOT']=A (Join-Path $script:profile 'system')
    $start.EnvironmentVariables['ANDROID_DATA']=A (Join-Path $script:profile 'data')
    $start.EnvironmentVariables['ANDROID_ART_ROOT']=A (Join-Path $script:profile 'art')
    $start.EnvironmentVariables['ANDROID_I18N_ROOT']=A (Join-Path $script:profile 'i18n')
    $start.EnvironmentVariables['ANDROID_TZDATA_ROOT']=A (Join-Path $RuntimeDir 'tzdata')
    $start.EnvironmentVariables['LOCALAPPDATA']=Join-Path $script:caseDir 'profile'
    $start.EnvironmentVariables['APPDATA']=Join-Path $script:caseDir 'roaming'
    $start.EnvironmentVariables['GESHER_LOGS']=A $script:logs
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
        if(-not $process.WaitForExit(45000)){$process.Kill();throw "Synthetic WebView lifecycle VM timed out: $label"}
        $text=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult()
        [IO.File]::WriteAllText((Join-Path $script:caseDir "$label.log"),$text,[Text.Encoding]::UTF8)
        if($process.ExitCode -ne 0){throw "Synthetic WebView lifecycle VM failed: $label"}
        return $text
    }finally{$process.Dispose()}
}
function Launcher([string]$label,[string[]]$arguments){Vm $label (@('com.gesher.app.Launcher')+$arguments)}
$cases=@(
    @{Name='disable-before-use';Classes=@('DisabledActivity');Markers=@('disable-before-use')},
    @{Name='disable-after-use';Classes=@('UsedActivity');Markers=@('disable-after-use')},
    @{Name='concurrent-use-disable';Classes=@('RaceActivity');Markers=@('concurrent-use-disable')},
    @{Name='suffix-isolation';Classes=@('SuffixAActivity','SuffixBActivity','DefaultActivity','AliasActivity');Markers=@('suffix-fixtureA','suffix-fixtureB','suffix-default','suffix-case-alias')},
    @{Name='invalid-suffix';Classes=@('InvalidSuffixActivity');Markers=@('invalid-suffix')}
)
if($OnlyCase){$cases=@($cases|Where-Object{$_.Name -eq $OnlyCase})}
if($ExpectBaselineFailure){$cases=@($cases|Where-Object{$_.Name -eq 'disable-before-use'})}
$results=@()
foreach($case in $cases){
    $script:caseDir=Join-Path $run $case.Name
    $script:profile=Join-Path $script:caseDir 'profile/Gesher'
    $script:logs=Join-Path $script:caseDir 'logs'
    $script:device=$null
    foreach($part in 'system/framework','system/etc','system/fonts','data','art','i18n/etc/icu'){
        New-Item -ItemType Directory -Path (Join-Path $script:profile $part) -Force|Out-Null
    }
    New-Item -ItemType Directory -Path $script:logs -Force|Out-Null
    $icu=@(Get-ChildItem -LiteralPath $RuntimeDir -File -Filter 'icudt*l.dat')
    if($icu.Count -ne 1){throw 'Exactly one ICU data file required'}
    Copy-Item -LiteralPath $icu[0].FullName -Destination (Join-Path $script:profile 'i18n/etc/icu')
    Copy-Item -LiteralPath (Join-Path $RuntimeDir 'framework-res.apk') -Destination (Join-Path $script:profile 'system/framework')
    Copy-Item -LiteralPath (Join-Path $RuntimeDir 'build.prop') -Destination (Join-Path $script:profile 'system')
    if(Test-Path -LiteralPath (Join-Path $RuntimeDir 'fonts/fonts.xml')){
        Copy-Item -LiteralPath (Join-Path $RuntimeDir 'fonts/fonts.xml') -Destination (Join-Path $script:profile 'system/etc')
        Get-ChildItem -LiteralPath (Join-Path $RuntimeDir 'fonts') -File -Filter '*.ttf'|Copy-Item -Destination (Join-Path $script:profile 'system/fonts')
    }
    $started=$false
    try{
        $null=Launcher 'install' @('install',(A $Apk))
        $started=$true
        foreach($line in [IO.File]::ReadAllLines((Join-Path $script:profile 'data/system/gesher-device'))){
            if($line -match '^device=(.+)$'){$script:device=$Matches[1].Trim()}
        }
        if(-not $script:device){throw 'No synthetic device namespace'}
        foreach($class in $case.Classes){
            $null=Launcher $class @('launch','com.gesher.webviewlifecycle','--activity',(".$class"),'--headless','--keep-task')
        }
        $all=(@(Get-ChildItem -LiteralPath $script:logs -File -Filter '*.log'|ForEach-Object{ReadLog $_.FullName}))-join "`n"
        $markers=@($all -split "`r?`n"|Where-Object{$_ -like '*viewlife: *'})
        [IO.File]::WriteAllLines((Join-Path $script:caseDir 'synthetic-markers.log'),$markers,[Text.Encoding]::UTF8)
        if($ExpectBaselineFailure){
            if(-not $all.Contains('viewlife: application disableBeforeUse=false providerInitiallyNull=false') `
                -or -not $all.Contains('viewlife: FAIL disable-before-use')){
                throw "Expected eager-provider baseline failure was not reproduced: $script:caseDir"
            }
        }else{
            if($all.Contains('viewlife: FAIL')){throw "WebView lifecycle fixture failed: $script:caseDir"}
            foreach($marker in $case.Markers){if(-not $all.Contains("viewlife: PASS $marker")){throw "Missing lifecycle result: $marker"}}
            if($case.Name -eq 'suffix-isolation'){
                $dirs=@(Get-ChildItem -LiteralPath (Join-Path $script:profile 'data') -Directory -Recurse -Filter 'app_webview*')
                $expected=@{app_webview_fixtureA='fixtureA';app_webview_fixtureB='fixtureB';app_webview='default'}
                if($dirs.Count -ne 3 -or @($dirs|ForEach-Object{$_.Parent.FullName}|Select-Object -Unique).Count -ne 1){throw 'Suffix profiles are not three siblings in the same real app data directory'}
                foreach($dir in $dirs){
                    if(-not $expected.ContainsKey($dir.Name) -or [IO.File]::ReadAllText((Join-Path $dir.FullName 'lifecycle-owner.txt'),[Text.Encoding]::UTF8) -ne $expected[$dir.Name]){throw 'Suffix marker isolation failed'}
                }
            }
        }
        $results+=[pscustomobject]@{name=$case.Name;passed=$true;expectedBaselineFailure=[bool]$ExpectBaselineFailure;directory=$script:caseDir}
        Write-Host "PASS WebView lifecycle $($case.Name) expectedBaselineFailure=$ExpectBaselineFailure"
    }finally{
        if($started){$null=Launcher 'shutdown' @('shutdown')}
    }
}
$artifacts=@('dalvikvm.exe','framework.jar','gesher-app.jar','libgesher_webview_bridge.so')|ForEach-Object{
    $p=Join-Path $RuntimeDir $_
    [pscustomobject]@{path=$p;sha256=(Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant()}
}
[pscustomobject]@{
    scope='Fresh ordinary APK processes, all headless; no accounts/network/WebView pages; native bridge runtime/configuration used only in suffix cases'
    expectedBaselineFailure=[bool]$ExpectBaselineFailure
    apkSha256=(Get-FileHash -LiteralPath $Apk -Algorithm SHA256).Hash.ToLowerInvariant()
    artifacts=$artifacts
    results=$results
}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $run 'validation.json') -Encoding UTF8
Write-Host "WebView lifecycle evidence: $run"
