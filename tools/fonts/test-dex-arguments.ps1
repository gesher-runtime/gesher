# Pure source/ArgumentList metadata only: no JVM/native process is started.
$ErrorActionPreference='Stop'
$path=Join-Path $PSScriptRoot 'run-dex-comparison.ps1'
$tokens=$null;$errors=$null
$tree=[Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors|Out-String)}
foreach($name in @('NoDrive','New-FontChildArguments')) {
  $nodes=@($tree.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst]},$true)|Where-Object Name -ceq $name)
  if($nodes.Count -ne 1){throw 'Missing/ambiguous argument helper'}
  . ([scriptblock]::Create($nodes[0].Extent.Text))
}
$fixtures='D:\metadata\fixtures-אב space'
$missingImage='D:\metadata\missing-אב space\boot.art'
$core=@('D:\metadata\core-אב space\core-oj.jar','D:\metadata\core-אב space\core-libart.jar','D:\metadata\core-אב space\core-icu4j.jar')
$tail=@('gesher.Native3StandaloneFontTest','/metadata/lib-אב space.so','set','/metadata/root-אב space','/metadata/fonts-אב space','/metadata/xml-אב space/fonts.xml')
$expected=@(
 '-Xbootclasspath:/metadata/core-אב space/core-oj.jar:/metadata/core-אב space/core-libart.jar:/metadata/core-אב space/core-icu4j.jar',
 '-Ximage:/metadata/missing-אב space/boot.art',
 '-Xint','-Xusejit:false','-verbose:image','-cp','/metadata/fixtures-אב space/native2-test.dex',
 'gesher.Native3StandaloneFontTest','/metadata/lib-אב space.so','set','/metadata/root-אב space','/metadata/fonts-אב space','/metadata/xml-אב space/fonts.xml'
)
function Exact-Arguments([string[]]$Received,[string[]]$Wanted) {
  if($Received.Count -ne $Wanted.Count){return $false}
  for($i=0;$i -lt $Wanted.Count;$i++){if($Received[$i] -cne $Wanted[$i]){return $false}}
  $true
}
[string[]]$actual=@(New-FontChildArguments $core $tail)
if(-not(Exact-Arguments $actual $expected)){throw ('Intended independent VM argv differs: '+($actual|ConvertTo-Json -Compress))}
$psi=[Diagnostics.ProcessStartInfo]::new('metadata-only-never-started.exe')
foreach($value in $actual){$psi.ArgumentList.Add($value)}
if(-not(Exact-Arguments @($psi.ArgumentList) $expected)){throw 'Actual ArgumentList changed values/count'}
# Reproduce the exact failed036a expression, not a reconstruction of the bug.
$old=git show '036a49bd9b594fbc6393108bde6947bcd5e6c9d2:tools/fonts/run-dex-comparison.ps1'
if($LASTEXITCODE -ne 0){throw 'Frozen failed source unavailable'}
$oldTree=[Management.Automation.Language.Parser]::ParseInput(($old -join "`n"),[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Failed-source parser error'}
$oldAssignment=@($oldTree.FindAll({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -ceq '$childArguments'},$true))
if($oldAssignment.Count -ne 1){throw 'Missing exact failed argv assignment'}
$Bcp=$core;$Tail=$tail
$null=Invoke-Expression $oldAssignment[0].Extent.Text
if($childArguments.Count -ne 7 -or -not $childArguments[0].Contains(' -Ximage:') -or (Exact-Arguments $childArguments $expected)){throw 'Actual failed036a argument collapse not reproduced'}
foreach($mutant in @(@{Values=@($actual -join ' ');Name='merged-options'},@{Values=@($actual[0].Split(':'))+$actual[1..12];Name='split-bcp'})) {
  if(Exact-Arguments $mutant.Values $expected){throw "Argument negative accepted: $($mutant.Name)"}
}
$managed=@($core)+@('D:\metadata\framework-אב space\framework.jar','D:\metadata\framework-אב space\gesher-app.jar')
$managedTail=@('gesher.Native3DefaultFontTest','--expect-uninitialized')
[string[]]$managedActual=@(New-FontChildArguments $managed $managedTail)
if($managedActual.Count -ne 9 -or $managedActual[0] -cne '-Xbootclasspath:/metadata/core-אב space/core-oj.jar:/metadata/core-אב space/core-libart.jar:/metadata/core-אב space/core-icu4j.jar:/metadata/framework-אב space/framework.jar:/metadata/framework-אב space/gesher-app.jar' -or $managedActual[7] -cne $managedTail[0] -or $managedActual[8] -cne $managedTail[1]){throw 'Managed before-comparison option shape changed'}
Write-Host 'FONT_DEX_ARGUMENTS_PASS old036a_count=7 intended_core_count=13 managed_count=9 actual_ArgumentList=EXACT spaces_unicode=RETAINED merged_options_split_bcp=REJECTED native_processes_started=0'
