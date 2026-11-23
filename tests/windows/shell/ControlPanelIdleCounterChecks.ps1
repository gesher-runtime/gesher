param([string] $FixturePath = (Join-Path $PSScriptRoot 'ControlPanelIdleChecks.ps1'))

$ErrorActionPreference = 'Stop'
# Execute the actual WPF fixture's snapshot statements and conditions in an
# isolated PowerShell script scope. These language checks also run on PS5.1
# before the real WPF lifecycle gate; they do not substitute for that gate.
$tokens = $null; $parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    [IO.Path]::GetFullPath($FixturePath), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
$script:Checks = 0
function Assert-Counter([bool] $Okay, [string] $Reason) {
    if (-not $Okay) { throw $Reason }
    $script:Checks++
}
function Get-CounterScenario([string] $Reason) {
    $commands = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
        $node.CommandElements[-1] -is [Management.Automation.Language.StringConstantExpressionAst] -and
        $node.CommandElements[-1].Value -eq $Reason
    }, $true))
    if ($commands.Count -ne 1) { throw "Expected one counter condition: $Reason" }
    $command = $commands[0]
    $snapshots = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and
        $node.Right.Extent.Text -eq '$script:Draws' -and
        $node.Extent.StartOffset -lt $command.Extent.StartOffset
    }, $true) | Sort-Object { $_.Extent.StartOffset })
    if ($snapshots.Count -eq 0) { throw "Missing counter snapshot: $Reason" }
    [pscustomobject]@{
        Snapshot = $snapshots[-1].Extent.Text
        Variable = $snapshots[-1].Left.Extent.Text
        Command = $command.Extent.Text
    }
}
function Invoke-CounterScenario($Scenario, [int] $Increment, [switch] $Aliased) {
    $snapshot = $Scenario.Snapshot
    $command = $Scenario.Command
    if ($Aliased) {
        # Recreate the original bug with the same executable condition. The
        # lower-case local spelling aliases $script:Draws in script scope.
        $snapshot = $snapshot.Replace($Scenario.Variable, '$draws')
        $command = $command.Replace($Scenario.Variable, '$draws')
    }
    $source = @'
function Assert-Idle([bool] $Okay, [string] $Reason) { $Okay }
function Pump-Until([scriptblock] $Condition, [string] $Reason) { & $Condition }
$script:Draws = 10
'@
    $source += "`n$snapshot`n" + '$script:Draws += ' + $Increment + "`n$command"
    $runner = [PowerShell]::Create()
    try {
        [void]$runner.AddScript($source)
        $result = @($runner.Invoke())
        if ($runner.HadErrors) { throw ($runner.Streams.Error | Out-String) }
        if ($result.Count -ne 1 -or $result[0] -isnot [bool]) {
            throw 'Counter scenario did not return exactly one Boolean result'
        }
        [bool]$result[0]
    } finally { $runner.Dispose() }
}

$recovery = Get-CounterScenario 'Visible missed-event recovery did not repair an unwatched file'
$beforeRecovery = Get-CounterScenario 'The over-budget path unexpectedly triggered a scan before recovery'
$burst = Get-CounterScenario 'Notification burst rebuilt the view without coalescing'

Assert-Counter (-not (Invoke-CounterScenario $recovery 0)) 'Recovery accepted a counter without progress'
Assert-Counter (Invoke-CounterScenario $recovery 1) 'Recovery rejected a real redraw after its snapshot'
Assert-Counter (Invoke-CounterScenario $beforeRecovery 0) 'An unchanged counter failed the pre-recovery guard'
Assert-Counter (-not (Invoke-CounterScenario $beforeRecovery 1)) 'The pre-recovery guard accepted an unexpected redraw'
Assert-Counter (Invoke-CounterScenario $burst 1) 'The burst guard rejected one coalesced redraw'
Assert-Counter (Invoke-CounterScenario $burst 3) 'The burst guard rejected its allowed upper bound'
Assert-Counter (-not (Invoke-CounterScenario $burst 4)) 'The burst guard accepted excessive redraws'

Assert-Counter (-not (Invoke-CounterScenario $recovery 1 -Aliased)) 'The old alias no longer reproduces the recovery false negative'
Assert-Counter (Invoke-CounterScenario $beforeRecovery 1 -Aliased) 'The old alias no longer reproduces the pre-recovery false positive'
Assert-Counter (Invoke-CounterScenario $burst 4 -Aliased) 'The old alias no longer reproduces the burst false positive'
Write-Host "PASS: control panel idle counter conditions ($script:Checks checks, actual fixture statements and alias controls)"
