# Windows host regression coverage for the production panel's account transport.
# The temporary CLI endpoint is an isolated fixture, not Android/ART evidence.
$ErrorActionPreference = 'Stop'
if ([Threading.Thread]::CurrentThread.ApartmentState -ne 'STA') {
    & powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File $PSCommandPath
    exit $LASTEXITCODE
}
$panel = Join-Path $PSScriptRoot '../../../tools/package/gesher-control-panel.ps1'
$tokens = $null; $parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile((Resolve-Path $panel), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
foreach ($name in @('ConvertFrom-AccountsOutput', 'Invoke-PanelAccounts', 'Start-PanelAccounts')) {
    $definition = $ast.Find({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $true)
    if (-not $definition) { throw "Missing production function: $name" }
    Invoke-Expression $definition.Extent.Text
}
function Assert-True($condition, [string] $message) { if (-not $condition) { throw $message } }
function Encode([string] $value) { [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($value)) }

$here = Join-Path ([IO.Path]::GetTempPath()) ('gesher-accounts-test-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $here
try {
    # Exercise the real ProcessStartInfo and Windows PowerShell -File path, including
    # its argument parsing and default OEM stdout encoding, with ASCII wire fields.
    @'
param([Parameter(ValueFromRemainingArguments)] [string[]] $Arguments)
if ($Arguments.Count -ne 4 -or $Arguments[0] -cne 'accounts' -or $Arguments[1] -cne 'remove-encoded') { exit 2 }
if ($Arguments[2] -ceq 'deny') { exit 1 }
[Console]::WriteLine("account64`t" + $Arguments[2] + "`t" + $Arguments[3])
exit 0
'@ | Set-Content -LiteralPath (Join-Path $here 'gesher-app.ps1') -Encoding UTF8
    $names = @('plain@example.test', ([string][char]0x5d0 + [char]0x4e2d),
        "tab`there`nline`rreturn", 'quotes"and\tail\', ' trailing ')
    foreach ($name in $names) {
        $type = "test.type`twith`nseparators"
        $worker = Start-PanelAccounts -arguments @('remove-encoded', (Encode $type), (Encode $name))
        try { $results = @($worker.PowerShell.EndInvoke($worker.Handle)) }
        finally { $worker.PowerShell.Dispose() }
        $result = $results[-1]
        Assert-True ($result.ExitCode -eq 0) 'Account subprocess failed'
        $decoded = @(ConvertFrom-AccountsOutput $result.Lines)
        Assert-True ($decoded.Count -eq 1 -and $decoded[0].Type -ceq $type -and $decoded[0].Name -ceq $name) 'Account identity changed'
    }
    foreach ($bad in @("account64`t!!!`tYWJj", "account64`t/w==`tYWJj",
            "account64`tYWJj`t", "account64`tYWJj`tYWJj`textra")) {
        $rejected = $false
        try { $null = ConvertFrom-AccountsOutput @($bad) } catch { $rejected = $true }
        Assert-True $rejected 'Malformed account identity accepted'
    }
    $worker = Start-PanelAccounts -arguments @('remove-encoded', 'deny', 'fixture')
    try { $results = @($worker.PowerShell.EndInvoke($worker.Handle)) }
    finally { $worker.PowerShell.Dispose() }
    Assert-True ($results[-1].ExitCode -eq 1) 'Removal failure incorrectly reported success'

    # Observe timeout propagation without waiting 16 minutes. Only this test worker's
    # endpoint is replaced; Start-PanelAccounts is the production extracted function.
    function Invoke-PanelAccounts([string[]] $arguments) { $script:AccountsTimeoutMs }
    foreach ($timeout in @(60000, 960000)) {
        $worker = if ($timeout -eq 60000) { Start-PanelAccounts -arguments @('list-encoded') }
            else { Start-PanelAccounts -arguments @('remove-encoded', 'type', 'name') -timeoutMs $timeout }
        try { $observed = @($worker.PowerShell.EndInvoke($worker.Handle)) }
        finally { $worker.PowerShell.Dispose() }
        Assert-True ($observed[-1] -eq $timeout) 'Incorrect account worker timeout'
    }

    # Exercise an actual WPF Button handler registered during a DispatcherTimer tick,
    # then invoked after that tick returns. ShowDialog's nested dispatcher keeps the
    # dialog function scope alive; no runspace closure or per-row loop capture is used.
    Add-Type -AssemblyName PresentationFramework
    function Test-DeferredAccountClick {
        $state = @{ Clicked = $false; Deadline = [DateTime]::UtcNow.AddSeconds(5); Failure = $null }
        $startQuery = { $state.Clicked = $true }
        $frame = [Windows.Threading.DispatcherFrame]::new()
        $button = [Windows.Controls.Button]::new()
        $button.Tag = [PSCustomObject]@{ Type = 'test.type'; Name = 'test.name' }
        $timer = [Windows.Threading.DispatcherTimer]::new()
        $timer.Interval = [TimeSpan]::FromMilliseconds(10)
        $timer.Add_Tick({
            if ([DateTime]::UtcNow -gt $state.Deadline) {
                $state.Failure = 'Deferred WPF click timed out'
                $frame.Continue = $false
                return
            }
            if (-not $state.Registered) {
                $state.Registered = $true
                $button.Add_Click({
                    param($sender, $event)
                    Assert-True ($sender.Tag.Name -ceq 'test.name') 'Deferred click lost its account identity'
                    & $startQuery
                    $frame.Continue = $false
                })
            } else {
                try { $button.RaiseEvent([Windows.RoutedEventArgs]::new([Windows.Controls.Button]::ClickEvent)) }
                catch { $state.Failure = $_; $frame.Continue = $false }
            }
        })
        try {
            $timer.Start()
            [Windows.Threading.Dispatcher]::PushFrame($frame)
        } finally { $timer.Stop() }
        if ($state.Failure) { throw $state.Failure }
        Assert-True $state.Clicked 'Deferred WPF click lost its dialog scope'
    }
    Test-DeferredAccountClick
    Write-Output 'PASS: panel account identities survive Windows PowerShell transport; invalid records and removal failures rejected; lifecycle timeouts propagated'
} finally {
    # The target is the exact directory created above, never supplied by callers.
    $resolved = (Resolve-Path -LiteralPath $here).Path
    if ((Split-Path -Parent $resolved).TrimEnd('\') -ne ([IO.Path]::GetTempPath()).TrimEnd('\') -or
            (Split-Path -Leaf $resolved) -notlike 'gesher-accounts-test-*') { throw 'Unexpected fixture cleanup path' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
