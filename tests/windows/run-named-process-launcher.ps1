# Real CLI exit status for a requested named process while a newer package sibling lives.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $RuntimeDir,
    [Parameter(Mandatory)][string] $OutputRoot,
    [string] $Apk,
    [switch] $ExpectBaselineFailure,
    [switch] $RequireBootImages
)
$ErrorActionPreference='Stop'
# A pwsh parent may omit this Windows PowerShell host's script modules.
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
Set-StrictMode -Version 2
$RuntimeDir=(Resolve-Path -LiteralPath $RuntimeDir).Path
if(-not $Apk){$Apk=Join-Path $RuntimeDir 'namedprocesslauncher.apk'}
$Apk=(Resolve-Path -LiteralPath $Apk).Path
$run=Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('named-launch-'+[guid]::NewGuid().ToString('N'))
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
$results=@()
try{
    $install=Launcher 'install' @('install',(A $Apk))
    if(-not $install.finished -or $install.exitCode -ne 0){throw 'Named fixture install failed'}
    $started=$true
    foreach($line in [IO.File]::ReadAllLines((Join-Path $profile 'data/system/gesher-device'))){if($line -match '^device=(.+)$'){$script:device=$Matches[1].Trim()}}
    if(-not $script:device){throw 'No synthetic device namespace'}
    $cases=@(@{name='zero';activity='.ExitZeroActivity';code=0;cli=0},@{name='nonzero';activity='.Exit17Activity';code=17;cli=1},@{name='min-int';activity='.ExitMinActivity';code=[int]::MinValue;cli=1})
    if($ExpectBaselineFailure){$cases=@($cases[0])}
    $appLog=Join-Path $logs 'com.gesher.namedprocesslauncher.log'
    foreach($case in $cases){
        $offset=(ReadLog $appLog).Length
        $result=Launcher $case.name @('launch','com.gesher.namedprocesslauncher','--activity',$case.activity,'--headless','--keep-task') 20000
        $deadline=[DateTime]::UtcNow.AddSeconds(10)
        do{
            $text=ReadLog $appLog
            $segment=if($text.Length -ge $offset){$text.Substring($offset)}else{$text}
            $worker=[regex]::Match($segment,"named-launch: worker-ready pid=([0-9]+) code=$($case.code) process=com\.gesher\.namedprocesslauncher:worker")
            $all=ReadLog $appLog
            $sibling=[regex]::Match($all,'named-launch: sibling-ready pid=([0-9]+) process=com\.gesher\.namedprocesslauncher')
            if($worker.Success -and $sibling.Success -and $segment.Contains("code=$($case.code)") -and $segment.Contains('named-launch: worker-exit pid=')){break}
            Start-Sleep -Milliseconds 30
        }while([DateTime]::UtcNow -lt $deadline)
        if(-not $worker.Success -or -not $sibling.Success -or $worker.Groups[1].Value -eq $sibling.Groups[1].Value){throw 'Real requested/sibling process evidence missing'}
        $siblingPid=[int]$sibling.Groups[1].Value
        if(-not(Get-Process -Id $siblingPid -ErrorAction SilentlyContinue)){throw 'Package sibling did not stay alive'}
        if($ExpectBaselineFailure){
            if($result.finished -and $result.exitCode -eq 0){throw 'Old package-wide waiter unexpectedly passed'}
            if(-not $segment.Contains('System.exit called, status: 0')){throw 'Baseline requested process did not exit0'}
        }else{
            if(-not $result.finished -or $result.exitCode -ne $case.cli -or -not $result.text.Contains("(exit code $($case.code))")){throw "CLI did not report exact requested process exit: $($case.name)"}
            if($result.text.Contains('no OS exit code')){throw 'CLI lost the known OS code'}
        }
        $results+=[pscustomobject]@{name=$case.name;osCode=$case.code;cliExitCode=$result.exitCode;cliFinished=$result.finished;liveSibling=$true;expectedBaselineFailure=[bool]$ExpectBaselineFailure}
        Write-Host "PASS named-process CLI $($case.name) baseline=$ExpectBaselineFailure"
    }
    if($RequireBootImages){
        Get-ChildItem -LiteralPath $logs -File|Where-Object{$_.Extension -eq '.log' -and ($_.Name -eq 'system_server.log' -or $_.Name.StartsWith('com.gesher.namedprocesslauncher'))}|ForEach-Object{
            if(-not (ReadLog $_.FullName).Contains($expectedBoot)){throw 'Child did not load complete images'}
        }
    }
    [pscustomobject]@{scope='Fresh headless ordinary APK: requested named worker exits while newer default sibling lives; exact0/17/0x80000000 OS status';expectedBaselineFailure=[bool]$ExpectBaselineFailure;requireBootImages=[bool]$RequireBootImages;results=$results;javaSha256=(Get-FileHash -LiteralPath (Join-Path $RuntimeDir 'gesher-app.jar') -Algorithm SHA256).Hash.ToLowerInvariant();apkSha256=(Get-FileHash -LiteralPath $Apk -Algorithm SHA256).Hash.ToLowerInvariant()}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $run 'validation.json') -Encoding UTF8
    Write-Host "Named-process CLI evidence: $run"
}finally{
    if($started){$null=Launcher 'shutdown' @('shutdown')}
}
