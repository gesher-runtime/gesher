# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# ART writes ordinary diagnostics to stderr ("setting boot class path" at INFO on
# every start). With that stream redirected - the Control Center's worker, a
# caller's 2>, a console without a visible window - Windows PowerShell 5.1 turns
# each line into an ErrorRecord. gesher-app.ps1 runs under ErrorActionPreference
# Stop, so the VM runs through Invoke-Dalvikvm (the production function, taken
# from the script): its exit code is the result, and a VM that cannot start at
# all still fails. A cmd.exe stand-in plays the VM, so this runs without a runtime.

param(
    [string] $AppScriptPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-app.ps1'),
    [string] $DexScriptPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher.ps1')
)

$ErrorActionPreference = 'Stop'
$script:checks = 0
function Assert-Stderr([bool] $condition, [string] $message) {
    if (-not $condition) { throw "FAIL: $message" }
    $script:checks++
}

$ast = [Management.Automation.Language.Parser]::ParseFile((Resolve-Path $AppScriptPath), [ref]$null, [ref]$null)
$function = $ast.Find({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Invoke-Dalvikvm' }, $true)
Assert-Stderr ($null -ne $function) 'gesher-app.ps1 defines Invoke-Dalvikvm'
. ([scriptblock]::Create($function.Extent.Text))

# Every VM run in gesher-app.ps1 goes through it (Start-Process runs redirect to files).
$direct = $ast.FindAll({ param($n)
    $n -is [Management.Automation.Language.CommandAst] -and $n.InvocationOperator -eq 'Ampersand' -and
    $n.CommandElements[0].Extent.Text -eq '$dalvikvm' }, $true) |
    Where-Object { $_.Extent.StartOffset -lt $function.Extent.StartOffset -or $_.Extent.EndOffset -gt $function.Extent.EndOffset }
Assert-Stderr (@($direct).Count -eq 0) "gesher-app.ps1 runs dalvikvm only through Invoke-Dalvikvm: $(@($direct | ForEach-Object { $_.Extent.Text }) -join '; ')"

$dalvikvm = Join-Path $env:SystemRoot 'System32\cmd.exe'
$info = 'I dalvikvm.exe: parsed_options.cc:697 setting boot class path to /x/core-oj.jar'
function Get-StandIn([int] $code) { @('/d', '/c', "echo $info 1>&2& echo vm output& exit $code") }

# The stand-in reproduces the failure: a direct run under Stop with stderr redirected throws.
$failure = $null
try { $standIn = Get-StandIn 0; & $dalvikvm @standIn 2>$null | Out-Null } catch { $failure = $_.Exception.Message }
Assert-Stderr ($failure -and $failure.Contains('setting boot class path')) "A direct run under Stop fails with the INFO line: $failure"

# A successful run: no error, its output reaches the caller, exit code 0.
$standIn = Get-StandIn 0
$output = Invoke-Dalvikvm @standIn 2>$null
Assert-Stderr ($LASTEXITCODE -eq 0) "A successful run exits 0: $LASTEXITCODE"
Assert-Stderr ((@($output) -join "`n").Contains('vm output')) 'The VM output reaches the caller'

# A failing run: its own exit code, not an exception.
$standIn = Get-StandIn 7
$null = Invoke-Dalvikvm @standIn 2>$null
Assert-Stderr ($LASTEXITCODE -eq 7) "A failing run keeps its exit code: $LASTEXITCODE"

# Arguments arrive unchanged through an array, including -X...:value options.
$standIn = @('/d', '/c', 'echo', '-Xbootclasspath:/a/b.jar:/c/d.jar', '-Xusejit:false', '--ez', 'key', 'true')
$echoed = (Invoke-Dalvikvm @standIn) -join ''
Assert-Stderr ($echoed -eq '-Xbootclasspath:/a/b.jar:/c/d.jar -Xusejit:false --ez key true') "Arguments pass unchanged: $echoed"

# A VM that cannot start fails instead of exiting with a stale or zero code.
$dalvikvm = Join-Path ([IO.Path]::GetTempPath()) ('gesher-missing-' + [Guid]::NewGuid().ToString('N') + '.exe')
$global:LASTEXITCODE = 0
$failure = $null
try { Invoke-Dalvikvm '-Xusejit:false' 2>$null } catch { $failure = $_.Exception.Message }
Assert-Stderr ($failure -and $failure.Contains('gesher-missing-')) "A missing VM fails under Stop: $failure"
# Outside a try block under Continue the command lookup error does not stop the
# script; the missing exit code does (a separate process: no enclosing try here).
$probe = Join-Path ([IO.Path]::GetTempPath()) ('gesher-stderr-probe-' + [Guid]::NewGuid().ToString('N') + '.ps1')
$probeText = "`$ErrorActionPreference = 'Continue'`n`$dalvikvm = '$dalvikvm'`n" + $function.Extent.Text +
    "`n`$global:LASTEXITCODE = 0`nInvoke-Dalvikvm '-Xusejit:false' 2>`$null`nexit 0`n"
[IO.File]::WriteAllText($probe, $probeText, [Text.UTF8Encoding]::new($true))
try {
    # This run's stderr is itself redirected: Continue, as Invoke-Dalvikvm does.
    $ErrorActionPreference = 'Continue'
    $probeOutput = & (Join-Path $PSHOME 'powershell.exe') -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $probe 2>&1 |
        ForEach-Object { "$_" }
    $probeCode = $LASTEXITCODE
} finally {
    $ErrorActionPreference = 'Stop'
    Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
}
Assert-Stderr ($probeCode -ne 0) "A missing VM fails under Continue: exit $probeCode"
Assert-Stderr ((@($probeOutput) -join "`n").Contains('could not start')) "A missing VM is named under Continue: $(@($probeOutput) -join ' ')"

# gesher.ps1 runs its VM the same way.
$dex = [IO.File]::ReadAllText((Resolve-Path $DexScriptPath))
$run = $dex.IndexOf("& (Join-Path `$here 'dalvikvm.exe')")
Assert-Stderr ($run -gt 0) 'gesher.ps1 runs dalvikvm.exe'
$prefix = $dex.Substring(0, $run)
Assert-Stderr ($prefix.LastIndexOf("`$ErrorActionPreference = 'Continue'") -gt $prefix.LastIndexOf("`$ErrorActionPreference = 'Stop'")) 'gesher.ps1 runs the VM under Continue'
Assert-Stderr ($dex.Substring($run).Contains('if ($null -eq $global:LASTEXITCODE)')) 'gesher.ps1 fails when the VM cannot start'

Write-Host "Native stderr checks passed ($script:checks)."
# The probes above leave non-zero native exit codes; this script's result is its checks.
exit 0
