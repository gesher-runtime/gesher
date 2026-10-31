$ErrorActionPreference = 'Stop'
$root = Join-Path $env:RUNNER_TEMP ('ci-processes-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $root | Out-Null
$failed = 0
$tokens = $null
$errors = $null
. "$pwd/tests/windows/helpers/GtestProcess.ps1"
. "$pwd/tests/windows/helpers/CloseProcessReaders.ps1"
foreach ($helper in @(@('20-libs.ps1', 'Invoke-CoreLibsTest'),
                      @('21-posix.ps1', 'Invoke-ArtGtest'),
                      @('50-services.ps1', 'RunTimed'),
                      @('44-native2.ps1', 'N2Run'),
                      @('50-services.ps1', 'StartTimed'),
                      @('50-services.ps1', 'WaitTimed'),
                      @('49-locale.ps1', 'LocaleStart'),
                      @('49-locale.ps1', 'LocaleWait'),
                      @('52-surfaces.ps1', 'SurfaceRun'),
                      @('76-media4-opensl.ps1', 'InvokeOpenSl'))) {
  $ast = [System.Management.Automation.Language.Parser]::ParseFile(
    "$pwd/tests/windows/tracks/$($helper[0])", [ref]$tokens, [ref]$errors)
  if ($errors.Count) { throw $errors[0] }
  $definition = $ast.FindAll({ param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst]
  }, $false) | Where-Object Name -eq $helper[1]
  if (@($definition).Count -ne 1) { throw "Expected exactly one actual helper $($helper[1])" }
  . ([scriptblock]::Create($definition.Extent.Text))
  if ($helper[1] -eq 'SurfaceRun') {
    # Compile the unchanged real SIGQUIT helper only inside CI.
    $signalHelper = $ast.FindAll({ param($node)
      $node -is [System.Management.Automation.Language.IfStatementAst] -and
      $node.Extent.Text.StartsWith("if (-not ('GesherSurfWin32'")
    }, $false)
    if (@($signalHelper).Count -ne 1) { throw 'Missing actual Surface SIGQUIT source' }
    . ([scriptblock]::Create($signalHelper.Extent.Text))
  }
}
# Additional original dependencies required by the GraphicsDex body.
. "$pwd/tests/windows/helpers/ProcessReaderHandles.ps1"
$noDriveAst = [System.Management.Automation.Language.Parser]::ParseFile(
  "$pwd/tests/windows/run-tests.ps1", [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw $errors[0] }
$noDrive = @($noDriveAst.FindAll({ param($node)
  $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'NoDrive'
}, $true))
if ($noDrive.Count -ne 1) { throw 'Expected exactly one actual NoDrive' }
. ([scriptblock]::Create($noDrive[0].Extent.Text))
. "$pwd/tests/windows/tracks/84-ci-processes.ps1"
Write-Host "$failed test(s) failed"
exit $failed
