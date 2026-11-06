# APK/APKS/XAPK icons on a fresh isolated profile; no app install, registry edits or real user data.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $RuntimeDir,
    [string] $FixtureDir,
    [string] $ServicesDex,
    [Parameter(Mandatory)][string] $OutputRoot
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version 2
$RuntimeDir=(Resolve-Path -LiteralPath $RuntimeDir).Path
if(-not $FixtureDir){$FixtureDir=$RuntimeDir}
$FixtureDir=(Resolve-Path -LiteralPath $FixtureDir).Path
if(-not $ServicesDex){$ServicesDex=Join-Path $RuntimeDir 'services-tests.dex'}
$ServicesDex=(Resolve-Path -LiteralPath $ServicesDex).Path
$work=Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('shell-icons-'+[guid]::NewGuid().ToString('N'))
$profile=Join-Path $work 'profile'
foreach($sub in 'system/framework','data','art','i18n/etc/icu','tmp','icons'){
    New-Item -ItemType Directory -Path (Join-Path $profile $sub) -Force|Out-Null
}
foreach($name in 'icon-base.apk','icon-density.apk','icon-bundle.apks','icon-bundle.xapk'){
    if(-not(Test-Path -LiteralPath (Join-Path $FixtureDir $name) -PathType Leaf)){throw "Missing icon fixture: $name"}
}
Copy-Item -LiteralPath (Join-Path $RuntimeDir 'framework-res.apk') -Destination (Join-Path $profile 'system/framework')
Copy-Item -LiteralPath (Join-Path $RuntimeDir 'build.prop') -Destination (Join-Path $profile 'system')
$icu=@(Get-ChildItem -LiteralPath $RuntimeDir -File -Filter 'icudt*l.dat')
if($icu.Count -ne 1){throw 'Exactly one ICU data file is required'}
Copy-Item -LiteralPath $icu[0].FullName -Destination (Join-Path $profile 'i18n/etc/icu')
$images=@(Get-ChildItem -LiteralPath (Join-Path $RuntimeDir 'framework/x86_64') -File|
    Where-Object{$_.Name -match '^boot(?:-core-libart|-core-icu4j|-framework|-gesher-app)?\.(art|oat|vdex)$'})
if($images.Count -ne 15){throw 'Complete matching five-image chain required'}
function A([string]$path){
    $value=[IO.Path]::GetFullPath($path)-replace '\\','/'
    if($value -notmatch '^([A-Za-z]):(.*)$'){throw 'Expected local Windows path'}
    if($Matches[1] -ieq $RuntimeDir.Substring(0,1)){return $Matches[2]}
    return "/mnt/$($Matches[1].ToLowerInvariant())$($Matches[2])"
}
function Q([string]$value){'"'+[regex]::Replace([regex]::Replace($value,'(\\*)"','$1$1\"'),'(\\+)$','$1$1')+'"'}
$bcp=(@('core-oj','core-libart','core-icu4j','framework','gesher-app')|ForEach-Object{A (Join-Path $RuntimeDir ($_+'.jar'))})-join ':'
$argsVm=@("-Djava.io.tmpdir=$(A (Join-Path $profile 'tmp'))","-Xbootclasspath:$bcp",
    "-Ximage:$(A (Join-Path $RuntimeDir 'framework/boot.art'))",'-Xint','-Xusejit:false',
    '-cp',(A $ServicesDex),'com.gesher.app.shell.IconBundleTest',
    (A $FixtureDir),(A (Join-Path $RuntimeDir 'lifecycle.apk')),(A (Join-Path $profile 'icons')))
$start=[Diagnostics.ProcessStartInfo]::new((Join-Path $RuntimeDir 'dalvikvm.exe'))
$start.Arguments=($argsVm|ForEach-Object{Q $_})-join ' '
$start.WorkingDirectory=$RuntimeDir
$start.UseShellExecute=$false;$start.CreateNoWindow=$true
$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
$start.EnvironmentVariables['ANDROID_ROOT']=A (Join-Path $profile 'system')
$start.EnvironmentVariables['ANDROID_DATA']=A (Join-Path $profile 'data')
$start.EnvironmentVariables['ANDROID_ART_ROOT']=A (Join-Path $profile 'art')
$start.EnvironmentVariables['ANDROID_I18N_ROOT']=A (Join-Path $profile 'i18n')
$start.EnvironmentVariables['ANDROID_TZDATA_ROOT']=A (Join-Path $RuntimeDir 'tzdata')
$start.EnvironmentVariables['LOCALAPPDATA']=Join-Path $profile 'local'
$start.EnvironmentVariables['APPDATA']=Join-Path $profile 'roaming'
$start.EnvironmentVariables['GESHER_REQUIRE_BOOT_IMAGE']='1'
$start.EnvironmentVariables['GESHER_SHORTCUTS']='0'
$start.EnvironmentVariables['GESHER_IP_GEOLOCATION']='0'
$start.EnvironmentVariables['GESHER_APP_WINDOW']='headless'
$start.EnvironmentVariables['GESHER_BINDER_UID']='1000';$start.EnvironmentVariables['GESHER_UID']='1000'
foreach($name in 'GESHER_BINDER','GESHER_MICROG_SPOOF'){$start.EnvironmentVariables.Remove($name)}
$process=[Diagnostics.Process]::new();$process.StartInfo=$start
$started=$false
try{
    if(-not $process.Start()){throw 'Cannot start icon fixture'}
    $started=$true
    $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    $finished=$process.WaitForExit(90000)
    if(-not $finished){$process.Kill();$null=$process.WaitForExit(5000)}
    if(-not $stdout.Wait(5000) -or -not $stderr.Wait(5000)){throw "Icon fixture output did not close: $work"}
    [IO.File]::WriteAllText((Join-Path $work 'fixture.out.log'),$stdout.Result,[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $work 'fixture.err.log'),$stderr.Result,[Text.UTF8Encoding]::new($false))
    $combined=$stdout.Result+"`n"+$stderr.Result
    if(-not $finished -or $process.ExitCode -ne 0 -or $combined -notmatch 'IconBundleTest all passed: (\d+) checks'){
        throw "Icon fixture failed: $work"
    }
    $checks=[int]$Matches[1]
    $marker='gesher: boot image loaded: 5 spaces (boot.art boot-core-libart.art boot-core-icu4j.art boot-framework.art boot-gesher-app.art)'
    if(-not $combined.Contains($marker) -or $combined.Contains('imageless')){throw 'Five-space boot not verified'}
    # Reuse the normal shell track's direct COM probe; no registry registration.
    $probe=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'tracks/55-shell.ps1') -Raw
    $begin=$probe.IndexOf('using System;',$probe.IndexOf('GesherShellExtProbe.Handler'))
    $end=$probe.IndexOf("'@",$begin)
    if($begin -lt 0 -or $end -lt 0){throw 'Existing shell COM probe missing'}
    $code=$probe.Substring($begin,$end-$begin).Replace('where T : Delegate','where T : class').Replace(
        'return Marshal.GetDelegateForFunctionPointer<T>(Marshal.ReadIntPtr(vtable, slot * IntPtr.Size));',
        'return (T)(object)Marshal.GetDelegateForFunctionPointer(Marshal.ReadIntPtr(vtable, slot * IntPtr.Size), typeof(T));')
    if(-not ('GesherShellExtProbe.Handler' -as [type])){Add-Type -TypeDefinition $code}
    $extension=Join-Path $work 'extension-only'
    $local=Join-Path $work 'native-local'
    $list=Join-Path $local 'Gesher/data/system/apk-icons.list'
    New-Item -ItemType Directory -Path $extension,(Split-Path $list) -Force|Out-Null
    Copy-Item -LiteralPath (Join-Path $RuntimeDir 'gesher_shellext.dll') -Destination $extension
    Copy-Item -LiteralPath (Join-Path $profile 'data/system/apk-icons.list') -Destination $list
    $previous=$env:LOCALAPPDATA
    try{
        $env:LOCALAPPDATA=$local
        foreach($name in 'icon-bundle.apks','icon-bundle.xapk'){
            $flags=[uint32]0
            $icon=[GesherShellExtProbe.Handler]::IconOf((Join-Path $extension 'gesher_shellext.dll'),(Join-Path $FixtureDir $name),[ref]$flags)
            if(-not(Test-Path -LiteralPath $icon) -or $icon.EndsWith('gesher.ico') -or ($flags -band 2) -eq 0){throw 'Native bundle icon cache lookup failed'}
            $checks++
        }
        $empty=Join-Path $work 'empty.apks';[IO.File]::WriteAllBytes($empty,[byte[]]@())
        $flags=[uint32]0
        $icon=[GesherShellExtProbe.Handler]::IconOf((Join-Path $extension 'gesher_shellext.dll'),$empty,[ref]$flags)
        if($icon -ne (Join-Path $extension 'gesher.ico')){throw 'Native empty-file fallback changed'}
        $checks++
    }finally{$env:LOCALAPPDATA=$previous}
    $inputs=@()
    foreach($path in @((Join-Path $RuntimeDir 'dalvikvm.exe'),(Join-Path $RuntimeDir 'framework.jar'),
            (Join-Path $RuntimeDir 'gesher-app.jar'),(Join-Path $RuntimeDir 'gesher_shellext.dll'),$ServicesDex,
            (Join-Path $RuntimeDir 'lifecycle.apk'))+@($images.FullName)+@('icon-base.apk','icon-density.apk','icon-bundle.apks','icon-bundle.xapk'|ForEach-Object{Join-Path $FixtureDir $_})){
        $inputs+=@{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()}
    }
    $receipt=@{scope='Synthetic resource-only icon fixtures; real ART pixels, bundle temp/cache, direct native COM lookup; no install/server/registry/user profile';passed=$true;checks=$checks;completeBootSpaces=5;inputs=$inputs;
        stdoutSha256=(Get-FileHash -LiteralPath (Join-Path $work 'fixture.out.log')).Hash.ToLowerInvariant();stderrSha256=(Get-FileHash -LiteralPath (Join-Path $work 'fixture.err.log')).Hash.ToLowerInvariant()}
    [IO.File]::WriteAllText((Join-Path $work 'validation.json'),($receipt|ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
    Write-Output "Shell icons all passed: $checks checks"
    Write-Output "Evidence: $work"
}finally{if($started -and -not $process.HasExited){$process.Kill()};$process.Dispose()}
