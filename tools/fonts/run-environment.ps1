# Explicit f78-runtime diagnostic; no new runtime build, fallback or env conformance claim.
param([Parameter(Mandatory)][string]$RuntimeDirectory,[Parameter(Mandatory)][string]$FixturesDirectory,[Parameter(Mandatory)][ValidateSet('elf','dex')][string]$Context)
$ErrorActionPreference='Stop'
if($env:GITHUB_ACTIONS -cne 'true'){throw 'Environment characterization execution is CI only'}
function Guest([string]$Path){($Path -replace '\\','/') -replace '^[A-Za-z]:',''}
function Emit-EnvRecord([string]$Stage,$Value){
 $payload=$Value|ConvertTo-Json -Depth 25 -Compress -EscapeHandling EscapeNonAscii
 $digest=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($payload))).ToLowerInvariant()
 $count=[Math]::Max(1,[int][Math]::Ceiling($payload.Length/320.0))
 for($part=0;$part -lt $count;$part++){
  $offset=$part*320;$chunk=$payload.Substring($offset,[Math]::Min(320,$payload.Length-$offset))
  Write-Host ($Stage+' '+(@{record_sha256=$digest;part=$part+1;parts=$count;payload=$chunk}|ConvertTo-Json -Compress))
 }
}
function New-EnvironmentArguments([string]$Abi,$Case){
 $values=[Collections.Generic.List[string]]::new()
 if($Context -ceq 'elf'){
  foreach($value in @('--elf',(Join-Path $fixtures "$Abi/font_env"),'fonts_env')){$values.Add($value)}
 }else{
  $values.Add('-Xbootclasspath:'+(($core|ForEach-Object{Guest $_})-join ':'));$values.Add('-Ximage:'+(Guest $missingImage))
  foreach($value in @('-Xint','-Xusejit:false','-verbose:image','-cp')){$values.Add($value)}
  $values.Add((Guest "$fixtures/font-env.dex"));$values.Add('gesher.Native3FontEnvironmentTest');$values.Add((Guest "$fixtures/$Abi/libfont_env.so"))
 }
 foreach($value in @($Context,$Case.Name,$Case.Mode,(Guest $Case.Root),(Guest $Case.Override),(Guest $Case.InitialRoot),(Guest $Case.InitialOverride),(Guest "$($Case.Root)/fonts"))){$values.Add($value)}
 $values.ToArray()
}
$runtime=(Resolve-Path -LiteralPath $RuntimeDirectory).Path;$fixtures=(Resolve-Path -LiteralPath $FixturesDirectory).Path
python tools/fonts/environment-assemble.py --verify --destination $runtime
if($LASTEXITCODE -ne 0){throw 'Diagnostic f78/data/translator source ownership failed'}
python tools/fonts/environment-fixtures.py verify --root $fixtures
if($LASTEXITCODE -ne 0){throw 'Current-source/attempt environment fixture receipt failed'}
$base=Join-Path $env:RUNNER_TEMP ('font-env-'+[guid]::NewGuid());$installed=Join-Path $base 'installed';$work=Join-Path $base 'unrelated-אב space'
if([IO.Path]::GetPathRoot($fixtures) -ine [IO.Path]::GetPathRoot($base)){throw 'Environment diagnostic uses explicit same-drive paths; other-drive is separate'}
foreach($tree in @($runtime,$fixtures)){
 if(@(Get-ChildItem -LiteralPath $tree -Recurse|Where-Object{$_.Attributes -band [IO.FileAttributes]::ReparsePoint}).Count){throw 'Linked diagnostic input'}
 if(@(Get-ChildItem -LiteralPath $tree -Recurse -File|Where-Object Extension -in '.art','.oat','.vdex').Count){throw 'Boot image supplied to explicit imageless diagnostic'}
}
$ascii=Join-Path $base 'root-ascii';$unicode=Join-Path $base 'root-אב space';$overrideAscii=Join-Path $base 'override-ascii';$overrideUnicode=Join-Path $base 'override-אב space'
foreach($dir in @($installed,$work,"$ascii/fonts","$ascii/etc","$unicode/fonts","$unicode/etc",$overrideAscii,$overrideUnicode,"$base/art","$base/data","$base/i18n/etc/icu")){New-Item -ItemType Directory -Path $dir -Force|Out-Null}
Copy-Item -Path "$runtime/*" -Destination $installed -Recurse
python tools/fonts/environment-assemble.py --verify --destination $installed
if($LASTEXITCODE -ne 0){throw 'Installed copy changed diagnostic runtime ownership'}
foreach($root in @($ascii,$unicode)){Copy-Item -Path "$installed/fonts/*.ttf" -Destination "$root/fonts";Copy-Item -LiteralPath "$installed/fonts/fonts.xml" -Destination "$root/etc/fonts.xml"}
Copy-Item -LiteralPath "$installed/icudt76l.dat" -Destination "$base/i18n/etc/icu/icudt76l.dat"
if((Get-FileHash -LiteralPath "$installed/icudt76l.dat").Hash -cne (Get-FileHash -LiteralPath "$base/i18n/etc/icu/icudt76l.dat").Hash){throw 'ICU copy changed bytes'}
$core=@('core-oj','core-libart','core-icu4j')|ForEach-Object{Join-Path $installed "$_.jar"}
$missingImage=Join-Path $base 'explicitly-absent-image/boot.art';if(Test-Path -LiteralPath $missingImage){throw 'Diagnostic image unexpectedly exists'}
$cases=@(
 @{Name='ascii-launch';Mode='launch-wide';Root=$ascii;Override=$overrideAscii;InitialRoot=$ascii;InitialOverride=$overrideAscii},
 @{Name='unicode-override-launch';Mode='launch-wide';Root=$ascii;Override=$overrideUnicode;InitialRoot=$ascii;InitialOverride=$overrideUnicode},
 @{Name='unicode-root-launch';Mode='launch-wide';Root=$unicode;Override=$overrideAscii;InitialRoot=$unicode;InitialOverride=$overrideAscii},
 @{Name='unicode-both-launch';Mode='launch-wide';Root=$unicode;Override=$overrideUnicode;InitialRoot=$unicode;InitialOverride=$overrideUnicode},
 @{Name='unicode-post-setenv';Mode='post-setenv';Root=$unicode;Override=$overrideUnicode;InitialRoot=$ascii;InitialOverride=$overrideAscii}
)
$failures=0;$observed=0;$preJava=0
foreach($abi in @('x86_64','arm64-v8a')){
 foreach($case in $cases){
  [string[]]$arguments=@(New-EnvironmentArguments $abi $case)
  $wantedCount=if($Context -ceq 'elf'){11}else{17}
  if($arguments.Count -ne $wantedCount){throw 'Environment diagnostic argument shape changed'}
  $exe=Join-Path $installed $(if($Context -ceq 'elf'){'gesher-exec.exe'}else{'dalvikvm.exe'})
  $psi=[Diagnostics.ProcessStartInfo]::new($exe);foreach($value in $arguments){$psi.ArgumentList.Add($value)}
  if($psi.ArgumentList.Count -ne $wantedCount){throw 'Actual environment ArgumentList count changed'}
  for($i=0;$i -lt $wantedCount;$i++){if($psi.ArgumentList[$i] -cne $arguments[$i]){throw 'Actual environment ArgumentList value changed'}}
  $psi.WorkingDirectory=$work;$psi.UseShellExecute=$false;$psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true
  foreach($key in @('ANDROID_PRODUCT_ROOT','GESHER_REQUIRE_BOOT_IMAGE')){$null=$psi.Environment.Remove($key)}
  $psi.Environment['ANDROID_ROOT']=Guest $case.InitialRoot;$psi.Environment['GESHER_WINDOWS_FONTS']=Guest $case.InitialOverride
  $psi.Environment['ANDROID_ART_ROOT']=Guest "$base/art";$psi.Environment['ANDROID_DATA']=Guest "$base/data";$psi.Environment['ANDROID_I18N_ROOT']=Guest "$base/i18n";$psi.Environment['ANDROID_TZDATA_ROOT']=Guest "$installed/tzdata";$psi.Environment['LD_LIBRARY_PATH']=Guest (Join-Path $fixtures $abi)
  foreach($key in @('ANDROID_ROOT','GESHER_WINDOWS_FONTS')){Emit-EnvRecord 'FONT_ENV_LAUNCH' @{abi=$abi;case=$case.Name;context=$Context;key=$key;utf16_units=@($psi.Environment[$key].ToCharArray()|ForEach-Object{([int]$_).ToString('x4')});fixture_mode=$case.Mode;runtime_role='BOUNDED_QUALIFIED_F78_DIAGNOSTIC'}}
  Emit-EnvRecord 'FONT_ENV_CHILD' @{abi=$abi;case=$case.Name;context=$Context;argv=$arguments;actual_argument_list=@($psi.ArgumentList);cwd=$work;runtime_source='f78bcbbb10050267374fa5b94318174f384a0b7e';runtime_run='41108241255';whole_runtime_run_green=$false;current_source_fresh_runtime=$false;supplied_images=0;production_image_policy='UNCHANGED';native_path_unicode='BMP_ONLY';font_fallback_qualification=$false}
  $child=[Diagnostics.Process]::Start($psi);$handle=$child.SafeHandle;$childId=$child.Id
  try{
   $stdout=$child.StandardOutput.ReadToEndAsync();$stderr=$child.StandardError.ReadToEndAsync();$timedOut=-not $child.WaitForExit(120000)
   if($timedOut){try{$child.Kill($true)}catch{};$null=$child.WaitForExit(5000)}
   $complete=$stdout.Wait(10000) -and $stderr.Wait(10000)
   if($timedOut -or -not $complete){throw 'Bounded environment diagnostic process/pipe failure'}
   $text=$stdout.Result+"`n"+$stderr.Result;Write-Host $text
   $strict=$case.Name -ceq 'ascii-launch' -or $case.Mode -ceq 'post-setenv'
   $javaEntered=$text.Contains('FONT_ENV_JAVA_ENTER');$nativeEntered=$text.Contains("FONT_ENV_NATIVE_ENTER context=$Context case=$($case.Name) ")
   $normal=$child.ExitCode -eq 0 -and $nativeEntered -and $text.Contains("FONT_ENV_OBSERVED context=$Context case=$($case.Name) observation_only=1 unicode_environment_conformance=false")
   if($Context -ceq 'dex' -and $normal){
    $wanted=($core|ForEach-Object{Guest $_})-join ':';$rows=[regex]::Matches($text,'(?m)^FONT_ENV_CORE_BCP ([^\r\n]*)\r?$');$received=if($rows.Count -eq 1){$rows[0].Groups[1].Value}else{$null}
    Emit-EnvRecord 'FONT_ENV_BCP_IDENTITY' @{expected=$wanted;received=$received;count=$rows.Count;exact=($rows.Count -eq 1 -and $wanted -ceq $received)}
    $normal=$javaEntered -and $rows.Count -eq 1 -and $wanted -ceq $received -and $text.Contains('FONT_ENV_CORE_CLASSES_ABSENT Typeface FontFamily SystemFontMap') -and $text.Contains('Attempting to fall back to imageless running') -and -not $text.Contains('gesher: boot image loaded:')
   }
   if($normal){$observed++}elseif(-not $strict -and $Context -ceq 'dex' -and -not $javaEntered -and -not $nativeEntered -and $child.ExitCode -ne 0){$preJava++}else{$failures++}
   Emit-EnvRecord 'FONT_ENV_OUTCOME' @{abi=$abi;case=$case.Name;context=$Context;exit=$child.ExitCode;java_entered=$javaEntered;native_entered=$nativeEntered;complete_native_observation=$normal;strict_control=$strict;pre_java_failure_observed=(!$strict -and $Context -ceq 'dex' -and !$javaEntered -and !$nativeEntered -and $child.ExitCode -ne 0);failure_cause='UNPROVED_WITHOUT_STAGE_EVIDENCE';observation_only=$true;unicode_environment_qualified=$false;font_fallback_qualified=$false}
  }finally{
   if(-not $child.HasExited){try{$child.Kill($true)}catch{};$null=$child.WaitForExit(5000)}
   $child.Dispose();Write-Host "FONT_ENV_CLEANUP pid=$childId handle_closed=$($handle.IsClosed)"
  }
 }
}
Emit-EnvRecord 'FONT_ENV_SUMMARY' @{context=$Context;observed=$observed;pre_java_failures=$preJava;hard_failures=$failures;unicode_environment_qualified=$false;font_fallback_qualified=$false;whole_f78_runtime_run_green=$false}
if($failures){throw "Environment diagnostic/positive controls failed: $failures"}
