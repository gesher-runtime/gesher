# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# The App info's special access switches (issues #310, #312): run the production
# functions that read and change them through `gesher-app special-access`. The device's
# side (SpecialAccess.java) has its own test (tests/dex-services SpecialAccessTest); here
# the launcher is a stand-in script, and no WPF window is opened.
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
foreach ($name in 'ConvertFrom-SpecialAccessOutput', 'Invoke-PanelSpecialAccess', 'Test-PanelPackageName',
        'Get-AppSpecialAccess', 'Set-AppSpecialAccess') {
    $functions = @($ast.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $true))
    if ($functions.Count -ne 1) { throw "Expected one production function: $name" }
    . ([scriptblock]::Create($functions[0].Extent.Text))
}
foreach ($variable in '$script:SpecialAccessLabels', '$script:SpecialAccessTimeoutMs') {
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
foreach ($key in 'cp_access_title', 'cp_access_unavailable', 'cp_access_failed') {
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
    ''))
Assert-Access ($parsed.Count -eq 2) "Only well-formed rows of known switches are read ($($parsed.Count))"
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
Write-Output 'gesher-app: the device is running'
Write-Output "special-access`t$($Arguments[2])`t$(if ($Arguments[3] -eq 'allow') { 'allowed' } else { 'denied' })"
exit 0
'@)
    Assert-Access ((Set-AppSpecialAccess 'com.example.app' 'android:system_alert_window' $true) -eq $true) 'An allowed answer through the process is read'
    Assert-Access ((Set-AppSpecialAccess 'com.example.app' 'android:system_alert_window' $false) -eq $false) 'A denied answer through the process is read'
    $script:SpecialAccessTimeoutMs = 1000
    [IO.File]::WriteAllText((Join-Path $here 'gesher-app.ps1'), "Start-Sleep -Seconds 20`n")
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $result = Invoke-PanelSpecialAccess @('com.example.app')
    Assert-Access ($result.ExitCode -eq -1 -and $watch.ElapsedMilliseconds -lt 15000) 'A launcher that does not answer is given up'
} finally {
    Start-Sleep -Milliseconds 200
    Remove-Item -LiteralPath $here -Recurse -Force -ErrorAction SilentlyContinue
}

# The App info dialog's switches in actual WPF: one per switch, the device's answer shown,
# a failed change reverted with its message. Windows only, on an STA thread.
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
    $script:window = [System.Windows.Window]::new()
    $window.ShowInTaskbar = $false; $window.ShowActivated = $false; $window.Opacity = 0
    $window.Width = 80; $window.Height = 80
    $window.Show()
    $script:SwitchCalls = [Collections.Generic.List[string]]::new()
    $script:Seen = @{ Labels = @(); Before = @(); After = @(); Error = $null; ErrorVisible = $null; ErrorAfter = $null; Title = $null }
    $timer = [System.Windows.Threading.DispatcherTimer]::new()
    $timer.Interval = [TimeSpan]::FromMilliseconds(100)
    $timer.Add_Tick({
        $dialog = @($window.OwnedWindows)[0]
        if (-not $dialog -or -not $dialog.IsLoaded) { return }
        $timer.Stop()
        try {
            $boxes = @($dialog.FindName('SwitchList').Children)
            $script:Seen.Title = $dialog.FindName('SwitchTitle').Text
            $script:Seen.Labels = @($boxes | ForEach-Object { [string]$_.Content })
            $script:Seen.Before = @($boxes | ForEach-Object { [bool]$_.IsChecked })
            # What a mouse click or the space key runs: ToggleButton.OnClick toggles, then raises Click.
            $onClick = [System.Windows.Controls.Primitives.ToggleButton].GetMethod('OnClick',
                [Reflection.BindingFlags]'Instance, NonPublic')
            foreach ($box in $boxes[0], $boxes[1]) { $null = $onClick.Invoke($box, @()) }
            $script:Seen.Error = $dialog.FindName('SwitchError').Text
            $script:Seen.ErrorVisible = [string]$dialog.FindName('SwitchError').Visibility
            $null = $onClick.Invoke($boxes[2], @())
            $script:Seen.After = @($boxes | ForEach-Object { [bool]$_.IsChecked })
            $script:Seen.ErrorAfter = [string]$dialog.FindName('SwitchError').Visibility
        } finally { $dialog.Close() }
    })
    $timer.Start()
    $answer = Show-ModernConfirmDialog -Title 'App' -Message 'Installed' -ConfirmText 'Uninstall' -CancelText 'Close' `
        -CancelIsDefault -Switches @(
            [PSCustomObject]@{ Op = 'android:manage_external_storage'; Allowed = $false },
            [PSCustomObject]@{ Op = 'android:system_alert_window'; Allowed = $false },
            [PSCustomObject]@{ Op = 'android:write_settings'; Allowed = $true }) `
        -OnSwitch {
            param($op, $allow)
            $script:SwitchCalls.Add("$op=$allow")
            if ($op -eq 'android:system_alert_window') { return $null }  # the device refused
            if ($op -eq 'android:write_settings') { return $true }       # the app holds it anyway
            return $allow
        }
    $window.Close()
    Assert-Access (@($answer).Count -eq 1 -and $answer -eq $false) 'Closing App info uninstalls nothing'
    Assert-Access ($script:Seen.Title -eq (Get-GesherText 'cp_access_title')) 'The switches have their heading'
    Assert-Access (($script:Seen.Labels -join '|') -eq ((Get-GesherText 'cp_access_all_files'), (Get-GesherText 'cp_access_overlay'),
        (Get-GesherText 'cp_access_write_settings') -join '|')) "One labeled switch each: $($script:Seen.Labels -join '|')"
    Assert-Access (($script:Seen.Before -join ',') -eq 'False,False,True') 'The switches start as the device answered'
    Assert-Access (($script:SwitchCalls -join ',') -eq 'android:manage_external_storage=True,android:system_alert_window=True,android:write_settings=False') "Each click asks for its switch: $($script:SwitchCalls -join ',')"
    Assert-Access (($script:Seen.After -join ',') -eq 'True,False,True') "Each switch shows the device's answer, a failure reverted: $($script:Seen.After -join ',')"
    Assert-Access ($script:Seen.ErrorVisible -eq 'Visible' -and $script:Seen.Error -eq (Get-GesherText 'cp_access_failed' @((Get-GesherText 'cp_access_overlay')))) 'A failed change says which switch'
    Assert-Access ($script:Seen.ErrorAfter -eq 'Collapsed') 'The next change clears the message'

    $script:Seen = @{ Error = $null; Count = $null }
    $timer = [System.Windows.Threading.DispatcherTimer]::new()
    $timer.Interval = [TimeSpan]::FromMilliseconds(100)
    $timer.Add_Tick({
        $dialog = @($window.OwnedWindows)[0]
        if (-not $dialog -or -not $dialog.IsLoaded) { return }
        $timer.Stop()
        try {
            $script:Seen.Error = $dialog.FindName('SwitchError').Text
            $script:Seen.Count = @($dialog.FindName('SwitchList').Children).Count
        } finally { $dialog.Close() }
    })
    $script:window = [System.Windows.Window]::new()
    $window.ShowInTaskbar = $false; $window.ShowActivated = $false; $window.Opacity = 0
    $window.Show()
    $timer.Start()
    $null = Show-ModernConfirmDialog -Title 'App' -Message 'Installed' -SwitchesUnavailable
    $window.Close()
    Assert-Access ($script:Seen.Count -eq 0 -and $script:Seen.Error -eq (Get-GesherText 'cp_access_unavailable')) 'An unreadable device shows why, with no switch'
} else {
    Write-Host 'SKIP: the WPF switch checks need Windows and an STA thread.'
}

Write-Host "PASS: $script:Checks control panel special access checks"
