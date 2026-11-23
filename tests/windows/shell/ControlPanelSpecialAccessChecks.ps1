# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# The App info's special access switches (issues #310, #312): run the production
# functions that read and change them through `gesher-app special-access`. The device's
# side (SpecialAccess.java) has its own test (tests/dex-services SpecialAccessTest); here
# the launcher is a stand-in script, and the WPF dialog runs with a scripted launcher.
param(
    [string] $PanelPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-control-panel.ps1'),
    [string] $UiMessagesPath = (Join-Path $PSScriptRoot '../../../tools/package/ui-messages.json')
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
$script:Checks = 0
function Assert-Access([bool] $value, [string] $message) {
    if (-not $value) { throw "FAIL: $message" }
    $script:Checks++
}

$tokens = $null; $parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    [IO.Path]::GetFullPath($PanelPath), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
foreach ($name in 'ConvertFrom-SpecialAccessOutput', 'Invoke-PanelSpecialAccess', 'Start-PanelSpecialAccess',
        'Test-PanelSpecialAccessDone', 'Receive-PanelSpecialAccess', 'Clear-PanelSpecialAccessWorkers',
        'Test-PanelPackageName', 'Test-PanelSpecialAccessOp', 'Read-AppSpecialAccessResult',
        'Read-SetSpecialAccessResult', 'Get-AppSpecialAccess', 'Set-AppSpecialAccess', 'ConvertFrom-PanelRequest',
        'Receive-PanelResultTokens', 'Write-PanelLog') {
    $functions = @($ast.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $true))
    if ($functions.Count -ne 1) { throw "Expected one production function: $name" }
    . ([scriptblock]::Create($functions[0].Extent.Text))
}
foreach ($variable in '$script:SpecialAccessLabels', '$script:SpecialAccessTimeoutMs', '$script:OrphanSpecialAccessWorkers') {
    $assignments = @($ast.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -eq $variable
    }, $true))
    if ($assignments.Count -ne 1) { throw "Expected one production assignment: $variable" }
    . ([scriptblock]::Create($assignments[0].Extent.Text))
}

# The switches the device can answer (SpecialAccess.KINDS), each with both translations.
$expected = 'android:manage_external_storage', 'android:system_alert_window', 'android:request_install_packages',
    'android:write_settings', 'android:get_usage_stats', 'android:manage_media'
Assert-Access ((@($script:SpecialAccessLabels.Keys) -join ',') -eq ($expected -join ',')) 'The panel knows exactly the device''s switches, in Settings'' order'
$catalog = Get-Content -LiteralPath $UiMessagesPath -Raw -Encoding UTF8 | ConvertFrom-Json
$source = [IO.File]::ReadAllText([IO.Path]::GetFullPath(
    (Join-Path (Split-Path -Parent $PanelPath) '../../app/java/com/gesher/server/SpecialAccess.java')))
foreach ($op in $expected) {
    $key = $script:SpecialAccessLabels[$op]
    $entry = $catalog.messages.$key
    Assert-Access ($null -ne $entry -and $entry.en -and $entry.he -and $entry.en -ne $entry.he) "Both languages label $op"
    $constant = 'OPSTR_' + $op.Substring('android:'.Length).ToUpperInvariant()
    Assert-Access ($source.Contains("AppOpsManager.$constant,")) "The device lists $op"
}
foreach ($key in 'cp_access_title', 'cp_access_unavailable', 'cp_access_failed', 'cp_access_loading', 'cp_access_fixed') {
    Assert-Access ($null -ne $catalog.messages.$key -and $catalog.messages.$key.he) "Both languages have $key"
}

# The device's lines among the launcher's other output.
$tab = "`t"
$parsed = @(ConvertFrom-SpecialAccessOutput @(
    'gesher-app: starting the device',
    "special-access${tab}android:manage_external_storage${tab}allowed",
    "special-access${tab}android:system_alert_window${tab}denied`r",
    "special-access${tab}android:camera${tab}allowed",
    "special-access${tab}android:write_settings${tab}granted",
    "Special-Access${tab}android:get_usage_stats${tab}allowed",
    "special-access${tab}android:manage_media${tab}allowed${tab}extra",
    "xspecial-access${tab}android:manage_media${tab}allowed",
    "special-access${tab}android:request_install_packages${tab}allowed${tab}fixed",
    ''))
Assert-Access ($parsed.Count -eq 3) "Only well-formed rows of known switches are read ($($parsed.Count))"
Assert-Access (-not $parsed[0].Fixed -and $parsed[2].Op -eq 'android:request_install_packages' -and $parsed[2].Allowed -and $parsed[2].Fixed) 'A fixed switch is read as fixed'
Assert-Access ($parsed[0].Op -eq 'android:manage_external_storage' -and $parsed[0].Allowed -eq $true) 'An allowed row reads allowed'
Assert-Access ($parsed[1].Op -eq 'android:system_alert_window' -and $parsed[1].Allowed -eq $false) 'A denied row (CRLF) reads denied'

# The launcher boundary: answers scripted per call, every call recorded.
$script:Calls = [Collections.Generic.List[string]]::new()
$script:Answer = $null
$production = ${function:Invoke-PanelSpecialAccess}
function Invoke-PanelSpecialAccess([string[]] $arguments) {
    $script:Calls.Add(($arguments -join ' '))
    return $script:Answer
}

foreach ($bad in '', 'a', 'com', 'bad name.app', 'com.example.app;calc', 'com.example.app -Command x', '1com.example',
        'com..example', 'com.example.', "com.example`tapp", ('a.' + ('b' * 260))) {
    Assert-Access ($null -eq (Get-AppSpecialAccess $bad)) "An invalid package is refused: $bad"
    Assert-Access ($null -eq (Set-AppSpecialAccess $bad 'android:manage_external_storage' $true)) "An invalid package is not changed: $bad"
}
foreach ($op in 'android:camera', 'android:manage_external_storage ', 'ANDROID:MANAGE_EXTERNAL_STORAGE', '', 'allow') {
    Assert-Access ($null -eq (Set-AppSpecialAccess 'com.example.app' $op $true)) "An unknown switch is refused: '$op'"
}
Assert-Access ($script:Calls.Count -eq 0) 'Refused input never reaches the launcher'

$script:Answer = [PSCustomObject]@{ ExitCode = 0; Lines = @("special-access${tab}android:manage_external_storage${tab}denied",
    "special-access${tab}android:request_install_packages${tab}allowed") }
$switches = Get-AppSpecialAccess 'com.example.app'
Assert-Access ($script:Calls[-1] -eq 'com.example.app') 'Reading asks the device about the package only'
Assert-Access ($null -ne $switches -and @($switches).Count -eq 2 -and -not $switches[0].Allowed -and $switches[1].Allowed) 'The switches are the device''s answer'
$script:Answer = [PSCustomObject]@{ ExitCode = 0; Lines = @('gesher-app: nothing') }
$none = Get-AppSpecialAccess 'com.example.app'
Assert-Access ($null -ne $none -and @($none).Count -eq 0) 'An app requesting none has an empty list, not a failure'
$script:Answer = [PSCustomObject]@{ ExitCode = 1; Lines = @("special-access${tab}android:manage_external_storage${tab}allowed") }
Assert-Access ($null -eq (Get-AppSpecialAccess 'com.example.app')) 'A failed launcher is not read as switches'

$script:Answer = [PSCustomObject]@{ ExitCode = 0; Lines = @("special-access${tab}android:manage_external_storage${tab}allowed") }
$state = Set-AppSpecialAccess 'com.example.app' 'android:manage_external_storage' $true
Assert-Access ($script:Calls[-1] -eq 'com.example.app android:manage_external_storage allow') 'Turning on asks the device to allow'
Assert-Access ($state -is [bool] -and $state) 'The device''s new state is returned'
$script:Answer = [PSCustomObject]@{ ExitCode = 0; Lines = @("special-access${tab}android:manage_external_storage${tab}denied") }
$state = Set-AppSpecialAccess 'com.example.app' 'android:manage_external_storage' $false
Assert-Access ($script:Calls[-1] -eq 'com.example.app android:manage_external_storage deny') 'Turning off asks the device to deny'
Assert-Access ($state -is [bool] -and -not $state) 'Denied is returned as false'
$script:Answer = [PSCustomObject]@{ ExitCode = 0; Lines = @("special-access${tab}android:manage_external_storage${tab}denied") }
$state = Set-AppSpecialAccess 'com.example.app' 'android:manage_external_storage' $true
Assert-Access ($state -is [bool] -and -not $state) 'The switch shows what the device answers, not what was asked'
$script:Answer = [PSCustomObject]@{ ExitCode = 0; Lines = @("special-access${tab}android:write_settings${tab}allowed") }
Assert-Access ($null -eq (Set-AppSpecialAccess 'com.example.app' 'android:manage_external_storage' $true)) 'An answer about another switch is a failure'
$script:Answer = [PSCustomObject]@{ ExitCode = 1; Lines = @() }
Assert-Access ($null -eq (Set-AppSpecialAccess 'com.example.app' 'android:manage_external_storage' $true)) 'A refused change is a failure'

# The production process boundary, with a stand-in gesher-app.ps1 that echoes its
# arguments among other output, as the launcher does.
Set-Item function:Invoke-PanelSpecialAccess $production
$here = Join-Path ([IO.Path]::GetTempPath()) ('gesher-special-access-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $here | Out-Null
$script:PanelLogFile = Join-Path $here 'logs/control-panel.log'
try {
    [IO.File]::WriteAllText((Join-Path $here 'gesher-app.ps1'), @'
param([Parameter(ValueFromRemainingArguments)] [string[]] $Arguments = @())
Write-Output 'gesher-app: the device is running'
Write-Output ("args:" + ($Arguments -join '|'))
[Console]::Error.WriteLine('a diagnostic on stderr')
# A well-formed row, but a failed command: the row must not be believed.
Write-Output "special-access`tandroid:manage_external_storage`tallowed"
exit 4
'@)
    $result = Invoke-PanelSpecialAccess @('com.example.app')
    Assert-Access ($result.ExitCode -eq 4) "The launcher's exit code is kept ($($result.ExitCode))"
    Assert-Access (@($result.Lines) -contains 'args:special-access|com.example.app') 'The command and package reach the launcher as separate arguments'
    $result = Invoke-PanelSpecialAccess @('com.example.app', 'android:manage_external_storage', 'allow')
    Assert-Access ($result.ExitCode -eq 4) 'Exit codes pass through'
    Assert-Access (@($result.Lines) -contains 'args:special-access|com.example.app|android:manage_external_storage|allow') 'The switch and choice reach the launcher'
    $state = Set-AppSpecialAccess 'com.example.app' 'android:manage_external_storage' $true
    Assert-Access ($null -eq $state) 'A nonzero launcher exit is a failure through the real process boundary'
    [IO.File]::WriteAllText((Join-Path $here 'gesher-app.ps1'), @'
param([Parameter(ValueFromRemainingArguments)] [string[]] $Arguments = @())
Start-Sleep -Milliseconds 1500
Write-Output 'gesher-app: the device is running'
Write-Output "special-access`t$($Arguments[2])`t$(if ($Arguments[3] -eq 'allow') { 'allowed' } else { 'denied' })"
exit 0
'@)
    Assert-Access ((Set-AppSpecialAccess 'com.example.app' 'android:system_alert_window' $true) -eq $true) 'An allowed answer through the process is read'
    Assert-Access ((Set-AppSpecialAccess 'com.example.app' 'android:system_alert_window' $false) -eq $false) 'A denied answer through the process is read'

    # The background worker the dialog uses: it returns at once and answers later.
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $worker = Start-PanelSpecialAccess @('com.example.app', 'android:write_settings', 'allow')
    Assert-Access ($watch.ElapsedMilliseconds -lt 1200 -and -not (Test-PanelSpecialAccessDone $worker)) "Starting a change does not wait for the launcher ($($watch.ElapsedMilliseconds) ms)"
    while (-not (Test-PanelSpecialAccessDone $worker) -and $watch.ElapsedMilliseconds -lt 60000) { Start-Sleep -Milliseconds 50 }
    $answer = Receive-PanelSpecialAccess $worker
    Assert-Access ($answer.ExitCode -eq 0 -and (Read-SetSpecialAccessResult $answer 'android:write_settings') -eq $true) 'The worker brings the launcher''s answer'
    $broken = [PSCustomObject]@{ PowerShell = [PowerShell]::Create(); Handle = $null }
    Assert-Access ((Receive-PanelSpecialAccess $broken).ExitCode -eq -1) 'A broken worker is a failure, not an exception'
    Assert-Access ((Test-Path -LiteralPath $script:PanelLogFile) -and ([IO.File]::ReadAllText($script:PanelLogFile) -match 'special access: ')) 'A broken worker is logged'
    # Orphans of a closed dialog are disposed once they finish.
    $script:OrphanSpecialAccessWorkers.Add((Start-PanelSpecialAccess @('com.example.app')))
    Clear-PanelSpecialAccessWorkers
    Assert-Access ($script:OrphanSpecialAccessWorkers.Count -eq 1) 'A running orphan is kept'
    $watch.Restart()
    while ($script:OrphanSpecialAccessWorkers.Count -and $watch.ElapsedMilliseconds -lt 60000) { Start-Sleep -Milliseconds 100; Clear-PanelSpecialAccessWorkers }
    Assert-Access ($script:OrphanSpecialAccessWorkers.Count -eq 0) 'A finished orphan is disposed'

    # A launcher that does not answer is given up, and its child (dalvikvm) ends with it.
    $script:SpecialAccessTimeoutMs = 1500
    $childFile = Join-Path $here 'child.pid'
    [IO.File]::WriteAllText((Join-Path $here 'gesher-app.ps1'), @"
`$child = Start-Process powershell -ArgumentList '-NoProfile','-Command','Start-Sleep -Seconds 60' -WindowStyle Hidden -PassThru
[IO.File]::WriteAllText('$childFile', [string]`$child.Id)
Start-Sleep -Seconds 60
"@)
    $watch.Restart()
    $result = Invoke-PanelSpecialAccess @('com.example.app')
    Assert-Access ($result.ExitCode -eq -1 -and $watch.ElapsedMilliseconds -lt 20000) 'A launcher that does not answer is given up'
    $childId = [int]([IO.File]::ReadAllText($childFile))
    Start-Sleep -Milliseconds 500
    $alive = Get-Process -Id $childId -ErrorAction SilentlyContinue
    if ($alive) { Stop-Process -Id $childId -Force -ErrorAction SilentlyContinue }
    Assert-Access (-not $alive) 'The launcher''s child process is ended with it'
    $script:SpecialAccessTimeoutMs = 120000
} finally {
    Start-Sleep -Milliseconds 200
    Remove-Item -LiteralPath $here -Recurse -Force -ErrorAction SilentlyContinue
}

# A request names one package; the results of requests that wait for the details are one
# file per token holding the package (ControlCenter.java), taken for the package shown.
Assert-Access ((ConvertFrom-PanelRequest 'com.example.app').Package -eq 'com.example.app') 'A request is read'
foreach ($bad in "com.example.app`t0123456789abcdef0123456789abcdef", '../evil', 'single', '') {
    Assert-Access ($null -eq (ConvertFrom-PanelRequest $bad)) "A malformed request is refused: $bad"
}
$script:PanelResultsDir = Join-Path ([IO.Path]::GetTempPath()) ('gesher-panel-results-' + [guid]::NewGuid().ToString('N'))
try {
    Assert-Access (@(Receive-PanelResultTokens 'com.example.app').Count -eq 0) 'No results directory, no tokens'
    New-Item -ItemType Directory -Path $script:PanelResultsDir | Out-Null
    $mine = '0123456789abcdef0123456789abcdef', 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
    foreach ($token in $mine) { [IO.File]::WriteAllText((Join-Path $script:PanelResultsDir $token), "com.example.app`n") }
    [IO.File]::WriteAllText((Join-Path $script:PanelResultsDir 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'), "com.example.other`n")
    [IO.File]::WriteAllText((Join-Path $script:PanelResultsDir 'not-a-token'), "com.example.app`n")
    [IO.File]::WriteAllText((Join-Path $script:PanelResultsDir 'cccccccccccccccccccccccccccccccc.tmp'), "com.example.app`n")
    $taken = @(Receive-PanelResultTokens 'com.example.app')
    Assert-Access ((@($taken | Sort-Object) -join ',') -eq (@($mine | Sort-Object) -join ',')) "The package's tokens are taken: $($taken -join ',')"
    Assert-Access (-not (Test-Path (Join-Path $script:PanelResultsDir $mine[0])) -and (Test-Path (Join-Path $script:PanelResultsDir 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'))) 'Taken files go; another app''s stay'
    Assert-Access (@(Receive-PanelResultTokens 'com.example.app').Count -eq 0) 'A token is taken once'
} finally { Remove-Item -LiteralPath $script:PanelResultsDir -Recurse -Force -ErrorAction SilentlyContinue }

# The App info dialog's switches in actual WPF, through the production background
# polling: shown at once while loading, a switch disabled while its change runs, the
# device's answer shown, a failed change reverted with its message, a fixed switch disabled.
# The launcher boundary is a scripted stand-in whose answers take a few polls.
if ($env:OS -eq 'Windows_NT' -and [Threading.Thread]::CurrentThread.ApartmentState -eq 'STA') {
    Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase
    $uiAst = [System.Management.Automation.Language.Parser]::ParseFile([IO.Path]::GetFullPath(
        (Join-Path (Split-Path -Parent $PanelPath) 'gesher-ui.ps1')), [ref]$tokens, [ref]$parseErrors)
    foreach ($pair in @(@($uiAst, 'Get-GesherText'), @($ast, 'Show-ModernConfirmDialog'))) {
        $definition = $pair[0].Find({ param($node)
            $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $pair[1]
        }, $false)
        . ([scriptblock]::Create($definition.Extent.Text))
    }
    $script:GesherUiCatalog = $catalog
    $script:GesherUiLanguage = 'en'
    $flowDirection = 'LeftToRight'
    $script:Started = [Collections.Generic.List[string]]::new()
    $script:QueryAnswer = $null
    function Start-PanelSpecialAccess([string[]] $arguments) {
        $script:Started.Add(($arguments -join ' '))
        $result = if ($arguments.Count -eq 1) { $script:QueryAnswer }
            elseif ($arguments[1] -eq 'android:system_alert_window') { [PSCustomObject]@{ ExitCode = 1; Lines = @() } }
            elseif ($arguments[1] -eq 'android:write_settings') { [PSCustomObject]@{ ExitCode = 0; Lines = @("special-access`tandroid:write_settings`tallowed") } }
            else { [PSCustomObject]@{ ExitCode = 0; Lines = @("special-access`t$($arguments[1])`t$(if ($arguments[2] -eq 'allow') { 'allowed' } else { 'denied' })") } }
        return [PSCustomObject]@{ Polls = 0; Result = $result }
    }
    function Test-PanelSpecialAccessDone($worker) { $worker.Polls++; return $worker.Polls -ge 4 }
    function Receive-PanelSpecialAccess($worker) { return $worker.Result }
    $script:OrphanSpecialAccessWorkers = [Collections.Generic.List[object]]::new()

    function New-OwnerWindow {
        $script:window = [System.Windows.Window]::new()
        $window.ShowInTaskbar = $false; $window.ShowActivated = $false; $window.Opacity = 0
        $window.Width = 80; $window.Height = 80
        $window.Show()
    }
    # Drives the open dialog step by step from a dispatcher timer; closes it when done.
    function Invoke-DialogProbe([scriptblock] $step) {
        $script:Probe = @{ Step = 0; Ticks = 0; Failure = $null }
        $script:ProbeStep = $step
        $script:ProbeTimer = [System.Windows.Threading.DispatcherTimer]::new()
        $script:ProbeTimer.Interval = [TimeSpan]::FromMilliseconds(50)
        $script:ProbeTimer.Add_Tick({
            $dialog = @($window.OwnedWindows)[0]
            if (-not $dialog -or -not $dialog.IsLoaded) { return }
            $script:Probe.Ticks++
            try {
                $done = & $script:ProbeStep $dialog
                if ($script:Probe.Ticks -gt 400) { $script:Probe.Failure = "timed out at step $($script:Probe.Step)"; $done = $true }
            } catch { $script:Probe.Failure = $_.Exception.Message; $done = $true }
            if ($done) { $script:ProbeTimer.Stop(); $dialog.Close() }
        })
        $script:ProbeTimer.Start()
        return $script:ProbeTimer
    }
    # What a mouse click or the space key runs: ToggleButton.OnClick toggles, then raises Click.
    $script:OnClick = [System.Windows.Controls.Primitives.ToggleButton].GetMethod('OnClick', [Reflection.BindingFlags]'Instance, NonPublic')

    $script:QueryAnswer = [PSCustomObject]@{ ExitCode = 0; Lines = @(
        "special-access`tandroid:manage_external_storage`tdenied",
        "special-access`tandroid:system_alert_window`tdenied",
        "special-access`tandroid:request_install_packages`tallowed`tfixed",
        "special-access`tandroid:write_settings`tallowed") }
    $script:Seen = @{}
    New-OwnerWindow
    $null = Invoke-DialogProbe {
        param($dialog)
        $boxes = @($dialog.FindName('SwitchList').Children)
        switch ($script:Probe.Step) {
            0 {
                $script:Seen.Loading = [string]$dialog.FindName('SwitchStatus').Visibility
                $script:Seen.LoadingText = $dialog.FindName('SwitchStatus').Text
                $script:Seen.BoxesWhileLoading = $boxes.Count
                $script:Probe.Step = 1
            }
            1 {
                if ($boxes.Count -lt 4) { return $false }
                $script:Seen.Title = $dialog.FindName('SwitchTitle').Text
                $script:Seen.StatusAfter = [string]$dialog.FindName('SwitchStatus').Visibility
                $script:Seen.Labels = @($boxes | ForEach-Object { [string]$_.Content })
                $script:Seen.Before = @($boxes | ForEach-Object { [bool]$_.IsChecked })
                $script:Seen.Enabled = @($boxes | ForEach-Object { [bool]$_.IsEnabled })
                $null = $script:OnClick.Invoke($boxes[0], @())
                $script:Seen.PendingDisabled = -not $boxes[0].IsEnabled
                $null = $script:OnClick.Invoke($boxes[0], @())  # ignored while pending
                $script:Probe.Step = 2
            }
            2 {
                if (-not $boxes[0].IsEnabled) { return $false }
                $null = $script:OnClick.Invoke($boxes[1], @())
                $script:Probe.Step = 3
            }
            3 {
                if (-not $boxes[1].IsEnabled) { return $false }
                $script:Seen.Error = $dialog.FindName('SwitchError').Text
                $script:Seen.ErrorVisible = [string]$dialog.FindName('SwitchError').Visibility
                $null = $script:OnClick.Invoke($boxes[3], @())
                $script:Probe.Step = 4
            }
            4 {
                if (-not $boxes[3].IsEnabled) { return $false }
                $script:Seen.After = @($boxes | ForEach-Object { [bool]$_.IsChecked })
                $script:Seen.ErrorAfter = [string]$dialog.FindName('SwitchError').Visibility
                return $true
            }
        }
        return $false
    }
    $answer = Show-ModernConfirmDialog -Title 'App' -Message 'Installed' -ConfirmText 'Uninstall' -CancelText 'Close' `
        -CancelIsDefault -SpecialAccessPackage 'com.example.app'
    $window.Close()
    Assert-Access ($null -eq $script:Probe.Failure) "The dialog probe ran: $($script:Probe.Failure)"
    Assert-Access (@($answer).Count -eq 1 -and $answer -eq $false) 'Closing App info uninstalls nothing'
    Assert-Access ($script:Seen.Loading -eq 'Visible' -and $script:Seen.LoadingText -eq (Get-GesherText 'cp_access_loading') -and $script:Seen.BoxesWhileLoading -eq 0) 'The dialog opens at once, loading'
    Assert-Access ($script:Seen.StatusAfter -eq 'Collapsed' -and $script:Seen.Title -eq (Get-GesherText 'cp_access_title')) 'The loading line gives way to the switches'
    $expectedLabels = (Get-GesherText 'cp_access_all_files'), (Get-GesherText 'cp_access_overlay'),
        (Get-GesherText 'cp_access_fixed' @((Get-GesherText 'cp_access_install'))), (Get-GesherText 'cp_access_write_settings')
    Assert-Access (($script:Seen.Labels -join '|') -eq ($expectedLabels -join '|')) "One labeled switch each: $($script:Seen.Labels -join '|')"
    Assert-Access (($script:Seen.Before -join ',') -eq 'False,False,True,True') 'The switches start as the device answered'
    Assert-Access (($script:Seen.Enabled -join ',') -eq 'True,True,False,True') 'A fixed switch cannot be changed'
    Assert-Access ($script:Seen.PendingDisabled) 'A switch is disabled while its change runs'
    Assert-Access (($script:Started -join ',') -eq 'com.example.app,com.example.app android:manage_external_storage allow,com.example.app android:system_alert_window allow,com.example.app android:write_settings deny') "Each click asks once for its switch: $($script:Started -join ',')"
    Assert-Access ($script:Seen.ErrorVisible -eq 'Visible' -and $script:Seen.Error -eq (Get-GesherText 'cp_access_failed' @((Get-GesherText 'cp_access_overlay')))) 'A failed change says which switch'
    Assert-Access (($script:Seen.After -join ',') -eq 'True,False,True,True') "Each switch shows the device's answer, a failure reverted: $($script:Seen.After -join ',')"
    Assert-Access ($script:Seen.ErrorAfter -eq 'Collapsed') 'The next change clears the message'

    # Closing with a change still running hands it to the live timer's cleanup.
    $script:Started.Clear()
    New-OwnerWindow
    $null = Invoke-DialogProbe {
        param($dialog)
        $boxes = @($dialog.FindName('SwitchList').Children)
        if ($boxes.Count -lt 4) { return $false }
        $null = $script:OnClick.Invoke($boxes[0], @())
        return $true
    }
    $null = Show-ModernConfirmDialog -Title 'App' -Message 'Installed' -SpecialAccessPackage 'com.example.app'
    $window.Close()
    Assert-Access ($null -eq $script:Probe.Failure -and $script:OrphanSpecialAccessWorkers.Count -eq 1) 'A change running at close is kept for cleanup'
    foreach ($i in 1..5) { Clear-PanelSpecialAccessWorkers }
    Assert-Access ($script:OrphanSpecialAccessWorkers.Count -eq 0) 'The cleanup disposes it once it answers'

    # A device that cannot answer, and an app requesting nothing.
    foreach ($case in @(@{ Answer = [PSCustomObject]@{ ExitCode = 3; Lines = @() }; Expect = 'unavailable' },
            @{ Answer = [PSCustomObject]@{ ExitCode = 0; Lines = @('gesher-app: nothing') }; Expect = 'none' })) {
        $script:QueryAnswer = $case.Answer
        $script:Seen = @{}
        New-OwnerWindow
        $null = Invoke-DialogProbe {
            param($dialog)
            if ($script:Probe.Ticks -lt 12) { return $false }
            $script:Seen.Panel = [string]$dialog.FindName('SwitchPanel').Visibility
            $script:Seen.Error = $dialog.FindName('SwitchError').Text
            $script:Seen.Count = @($dialog.FindName('SwitchList').Children).Count
            return $true
        }
        $null = Show-ModernConfirmDialog -Title 'App' -Message 'Installed' -SpecialAccessPackage 'com.example.app'
        $window.Close()
        if ($case.Expect -eq 'unavailable') {
            Assert-Access ($script:Seen.Count -eq 0 -and $script:Seen.Error -eq (Get-GesherText 'cp_access_unavailable')) 'An unreadable device shows why, with no switch'
        } else {
            Assert-Access ($script:Seen.Panel -eq 'Collapsed' -and $script:Seen.Count -eq 0) 'An app requesting no special access shows no switch section'
        }
    }
} else {
    Write-Host 'SKIP: the WPF switch checks need Windows and an STA thread.'
}

Write-Host "PASS: $script:Checks control panel special access checks"
