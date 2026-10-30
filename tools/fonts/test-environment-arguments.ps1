# Source and ProcessStartInfo metadata only. Never start a native/JVM process.
$ErrorActionPreference='Stop'
$createdDrive=-not (Get-PSDrive -Name D -ErrorAction SilentlyContinue)
if($createdDrive){$null=New-PSDrive -Name D -PSProvider FileSystem -Root ([IO.Path]::GetTempPath())}
try {
$tokens=$null;$errors=$null
$tree=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'run-environment.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors|Out-String)}
foreach($name in @('Guest','New-EnvironmentArguments')){
 $nodes=@($tree.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst]},$true)|Where-Object Name -ceq $name)
 if($nodes.Count -ne 1){throw 'Missing/ambiguous environment argument helper'}
 . ([scriptblock]::Create($nodes[0].Extent.Text))
}
$fixtures='D:\metadata\fixture-אב space';$missingImage='D:\metadata\absent image\boot.art'
$core=@('core-oj','core-libart','core-icu4j')|ForEach-Object{"D:\metadata\core-אב space\$_.jar"}
$case=@{Name='unicode-post-setenv';Mode='post-setenv';Root='D:\metadata\root-אב space';Override='D:\metadata\override-אב space';InitialRoot='D:\metadata\ascii';InitialOverride='D:\metadata\override-ascii'}
function Exact([string[]]$Actual,[string[]]$Wanted){
 if($Actual.Count -ne $Wanted.Count){return $false}
 for($i=0;$i -lt $Actual.Count;$i++){if($Actual[$i] -cne $Wanted[$i]){return $false}}
 $true
}
$tail=@('unicode-post-setenv','post-setenv','/metadata/root-אב space','/metadata/override-אב space','/metadata/ascii','/metadata/override-ascii','/metadata/root-אב space/fonts')
$Context='elf';[string[]]$elf=@(New-EnvironmentArguments 'arm64-v8a' $case)
$expectedElf=@('--elf',(Join-Path $fixtures 'arm64-v8a/font_env'),'fonts_env','elf')+$tail
if(-not(Exact $elf $expectedElf) -or $elf.Count -ne 11){throw 'ELF host argv or nine-value guest argc changed'}
$Context='dex';[string[]]$dex=@(New-EnvironmentArguments 'x86_64' $case)
$expectedDex=@('-Xbootclasspath:/metadata/core-אב space/core-oj.jar:/metadata/core-אב space/core-libart.jar:/metadata/core-אב space/core-icu4j.jar','-Ximage:/metadata/absent image/boot.art','-Xint','-Xusejit:false','-verbose:image','-cp','/metadata/fixture-אב space/font-env.dex','gesher.Native3FontEnvironmentTest','/metadata/fixture-אב space/x86_64/libfont_env.so','dex')+$tail
if(-not(Exact $dex $expectedDex) -or $dex.Count -ne 17){throw 'DEX individual options, one three-jar BCP or nine Java values changed'}
foreach($actual in @(@{Values=$elf;Wanted=$expectedElf},@{Values=$dex;Wanted=$expectedDex})){
 $psi=[Diagnostics.ProcessStartInfo]::new('metadata-only-never-started.exe')
 foreach($value in $actual.Values){$psi.ArgumentList.Add($value)}
 if(-not(Exact @($psi.ArgumentList) $actual.Wanted)){throw 'Actual ArgumentList changed independent values'}
 $psi.Environment['ANDROID_ROOT']='/metadata/root-אב space';$psi.Environment['GESHER_WINDOWS_FONTS']='/metadata/override-אב space'
 foreach($entry in @(@{Key='ANDROID_ROOT';Expected='/metadata/root-אב space'},@{Key='GESHER_WINDOWS_FONTS';Expected='/metadata/override-אב space'})){
  if($psi.Environment[$entry.Key] -cne $entry.Expected -or -not $psi.Environment[$entry.Key].Contains([char]0x5d0)){throw 'Initial UTF16 child environment lost Hebrew or spaces'}
 }
 if(Exact @($actual.Values -join ' ') $actual.Wanted){throw 'Merged argv negative accepted'}
 $split=@($actual.Values[0].Split(':'))+$actual.Values[1..($actual.Values.Count-1)]
 if($actual.Values[0].StartsWith('-Xbootclasspath:') -and (Exact $split $actual.Wanted)){throw 'Split BCP option negative accepted'}
}
Write-Host 'FONT_ENV_ARGUMENTS_PASS elf_host=11 elf_guest=9 dex_host=17 java_values=9 exact_three_jar_BCP=1 initial_UTF16_Hebrew_spaces=1 native_processes_started=0'
} finally {if($createdDrive){Remove-PSDrive -Name D}}
