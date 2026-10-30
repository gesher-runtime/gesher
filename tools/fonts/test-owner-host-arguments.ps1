# Parse/exercise only managed ProcessStartInfo metadata; NEVER start a process.
$ErrorActionPreference='Stop';$tokens=$null;$errors=$null
$tree=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'run-owner-host.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors|Out-String)}
$functions=@($tree.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst]},$true)|Where-Object Name -ceq 'New-OwnerHostStartInfo')
if($functions.Count -ne 1){throw 'Missing/ambiguous host owner argument builder'}
. ([scriptblock]::Create($functions[0].Extent.Text))
foreach($mode in @('wide','missing','empty','invalid-wide','supplementary')){
 $psi=New-OwnerHostStartInfo 'D:\metadata\fixture-אב space.exe' $mode 'D:\metadata\work space'
 if($psi.FileName -cne 'D:\metadata\fixture-אב space.exe' -or $psi.ArgumentList.Count -ne 1 -or $psi.ArgumentList[0] -cne $mode -or $psi.WorkingDirectory -cne 'D:\metadata\work space' -or $psi.UseShellExecute -or !$psi.RedirectStandardOutput -or !$psi.RedirectStandardError){throw 'Actual managed argv/cwd/pipe metadata differs'}
 foreach($key in @('ANDROID_ROOT','GESHER_WINDOWS_FONTS')){
  $present=$psi.Environment.ContainsKey($key);$value=$psi.Environment[$key]
  if($mode -ceq 'missing'){if($present){throw 'Missing key became present'}}
  elseif(!$present){throw 'Present key became missing'}
  elseif($mode -ceq 'empty' -and $value -cne ''){throw 'Present empty value changed'}
  elseif($mode -ceq 'invalid-wide' -and ($value.Length -ne 1 -or [int]$value[0] -ne 0xd800)){throw 'Unpaired UTF16 changed before child startup'}
  elseif($mode -ceq 'wide' -and $value -cne 'initial-אב space'){throw 'BMP Hebrew/spaces changed'}
  elseif($mode -ceq 'supplementary' -and $value -cne ('initial-'+[char]::ConvertFromUtf32(0x1f600)+' space')){throw 'Supplementary scalar changed'}
 }
}
Write-Host 'FONT_OWNER_HOST_ARGUMENT_METADATA_PASS modes=5 exact_ArgumentList=1 missing_empty_distinct=1 raw_UTF16_BMP_SMP_unpaired=1 actual_child_transport_qualification=PENDING native_processes_started=0'
