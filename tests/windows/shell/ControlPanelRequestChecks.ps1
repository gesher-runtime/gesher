param([string] $PanelPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-control-panel.ps1'))

$ErrorActionPreference = 'Stop'
if ($env:OS -eq 'Windows_NT') { Add-Type -AssemblyName WindowsBase, PresentationFramework }
else {
    # Only the enum used by the production request dispatcher; this is not WPF validation.
    Add-Type 'namespace System.Windows { public enum WindowState { Normal, Minimized, Maximized } }'
}
# An app's "App info" (ADR 0087): the system server writes a package to
# data/system/control-panel-request and opens the Control Center. Execute the production
# request functions without a WPF window; the window and the dialog are stand-ins here.
$tokens = $null; $parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    [IO.Path]::GetFullPath($PanelPath), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
foreach ($name in 'Read-PanelRequest', 'Get-AppInstallTimes', 'Invoke-PanelRequest', 'ConvertFrom-PanelRequest',
        'Receive-PanelResultTokens', 'Set-PanelShowing', 'Add-PanelShowingTokens', 'Write-PanelLog',
        'ConvertFrom-QueuedPanelRequest', 'Import-LegacyPanelRequest', 'Receive-PanelRequest') {
    $functions = @($ast.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $true))
    if ($functions.Count -ne 1) { throw "Expected one production function: $name" }
    . ([scriptblock]::Create($functions[0].Extent.Text))
}

foreach ($variable in '$script:ShowingPackage', '$script:ShowingTokens') {
    $assignments = @($ast.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -eq $variable
    }, $true) | Where-Object { $_.Parent.Parent -is [System.Management.Automation.Language.ScriptBlockAst] })
    if ($assignments.Count -ne 1) { throw "Expected one production assignment: $variable" }
    . ([scriptblock]::Create($assignments[0].Extent.Text))
}
$script:Checks = 0
function Assert-Request([bool] $value, [string] $message) {
    if (-not $value) { throw $message }
    $script:Checks++
}

if ($env:OS -eq 'Windows_NT') {
# One panel: run the production startup lines (single-panel decision) in child processes,
# as a second start does, with the first panel's mutex held here.
$text = [IO.File]::ReadAllText([IO.Path]::GetFullPath($PanelPath))
$start = $text.IndexOf('$script:PanelReopen = ')
$end = $text.IndexOf("`n}", $text.IndexOf('if (-not $script:PanelInstanceCreated -and -not $script:PanelReopen)'))
if ($start -lt 0 -or $end -lt 0) { throw 'The single-panel startup block was not found' }
$startup = $text.Substring($start, $end + 2 - $start)
$suffix = [guid]::NewGuid().ToString('N')
$runtimeHelper = Join-Path (Split-Path -Parent ([IO.Path]::GetFullPath($PanelPath))) 'gesher-runtime-files.ps1'
. $runtimeHelper
$probeRoot = Join-Path ([IO.Path]::GetTempPath()) "gesher-panel-profile-$suffix"
$instanceName = Get-GesherControlPanelName $probeRoot
$startup = ". '" + $runtimeHelper.Replace("'", "''") + "'`n`$root = '" + $probeRoot.Replace("'", "''") + "'`n" + $startup
$probe = Join-Path ([IO.Path]::GetTempPath()) "gesher-panel-start-$suffix.ps1"
[IO.File]::WriteAllText($probe, '$root = $PSScriptRoot' + "`n" + $startup + "`nexit 7`n")
$created = $false
$mutex = [System.Threading.Mutex]::new($false, $instanceName, [ref] $created)
$show = [System.Threading.EventWaitHandle]::new($false, [System.Threading.EventResetMode]::AutoReset, ($instanceName + '.Show'))
try {
    Assert-Request $created 'The check could not create its panel mutex'
    & powershell -NoProfile -ExecutionPolicy Bypass -File $probe
    Assert-Request ($LASTEXITCODE -eq 0) "A second panel did not exit (exit $LASTEXITCODE)"
    Assert-Request ($show.WaitOne(2000)) 'A second panel did not signal the first'
    $env:GESHER_PANEL_REOPEN = '1'
    try { & powershell -NoProfile -ExecutionPolicy Bypass -File $probe } finally { Remove-Item Env:GESHER_PANEL_REOPEN }
    Assert-Request ($LASTEXITCODE -eq 7) "Reopen was stopped by the open panel (exit $LASTEXITCODE)"
    Assert-Request (-not $show.WaitOne(0)) 'Reopen signalled the old panel'
    $mutex.Dispose(); $show.Dispose(); $mutex = $null
    & powershell -NoProfile -ExecutionPolicy Bypass -File $probe
    Assert-Request ($LASTEXITCODE -eq 7) "A start after the panel closed did not open (exit $LASTEXITCODE)"
} finally {
    if ($mutex) { $mutex.Dispose(); $show.Dispose() }
    Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
}

}

$root = Join-Path ([IO.Path]::GetTempPath()) ("gesher-panel-request-" + [guid]::NewGuid())
New-Item -ItemType Directory -Force (Join-Path $root 'data/system') | Out-Null
try {
    $script:PanelRequestFile = Join-Path $root 'data/system/control-panel-request'
    $script:PanelResultsDir = Join-Path $root 'data/system/control-panel-results'
    $script:PanelRequestsDir = Join-Path $root 'data/system/control-panel-requests'
    $script:PanelShowingFile = Join-Path $root 'data/system/control-panel-showing'
    $script:PanelLogFile = Join-Path $root 'logs/control-panel.log'

    # A request is read once and leaves nothing behind.
    Assert-Request ($null -eq (Read-PanelRequest)) 'A missing request was read'
    [IO.File]::WriteAllText($script:PanelRequestFile, "com.example.app`n")
    Assert-Request ((Read-PanelRequest) -eq 'com.example.app') 'The request package was not read'
    Assert-Request (-not (Test-Path -LiteralPath $script:PanelRequestFile)) 'The request was not taken'
    Assert-Request (@(Get-ChildItem -LiteralPath (Split-Path $script:PanelRequestFile) -Filter '*.taken').Count -eq 0) 'The taken request was left behind'
    Assert-Request ($null -eq (Read-PanelRequest)) 'A taken request was read twice'

    # Deterministically interleave two consumers as during Reopen: the first has moved
    # its request but not read it when the second takes a newly published request.
    $script:InjectNextRequest = $true
    $script:NestedRequest = $null
    function Move-Item {
        param([string] $LiteralPath, [string] $Destination, [switch] $Force, $ErrorAction)
        Microsoft.PowerShell.Management\Move-Item -LiteralPath $LiteralPath -Destination $Destination -Force:$Force -ErrorAction Stop
        if ($script:InjectNextRequest) {
            $script:InjectNextRequest = $false
            [IO.File]::WriteAllText($script:PanelRequestFile, "com.example.second`n")
            $script:NestedRequest = Read-PanelRequest
        }
    }
    try {
        [IO.File]::WriteAllText($script:PanelRequestFile, "com.example.first`n")
        Assert-Request ((Read-PanelRequest) -eq 'com.example.first') 'A concurrent consumer replaced the first request'
        Assert-Request ($script:NestedRequest -eq 'com.example.second') 'A concurrent consumer lost the second request'
        Assert-Request (@(Get-ChildItem -LiteralPath (Split-Path $script:PanelRequestFile) -Filter '*.taken').Count -eq 0) 'Concurrent consumers left taken files behind'
    } finally { Remove-Item Function:Move-Item }

    # packages.list times (ms since 1970) become local dates; unknown packages have none.
    [IO.File]::WriteAllText((Join-Path $root 'data/system/packages.list'),
        "# package`tcode path`tfirst install`tlast update`ncom.example.app`t/data/app/x`t1700000000000`t1700000600000`n")
    $times = Get-AppInstallTimes 'com.example.app'
    Assert-Request ($times.Count -eq 2 -and $times[0] -and $times[1] -and $times[0] -ne $times[1]) 'Install times were not read'
    Assert-Request ($null -eq (Get-AppInstallTimes 'com.example.other')) 'An unknown package had times'

    # Invoke-PanelRequest: raises the window; only a package name reaches the dialog.
    $script:IsBusy = $false
    $script:ShowingAppInfo = $false
    $script:PanelShowEvent = [System.Threading.EventWaitHandle]::new($false, [System.Threading.EventResetMode]::AutoReset)
    $script:Shown = [System.Collections.Generic.List[string]]::new()
    $window = [PSCustomObject]@{ WindowState = [System.Windows.WindowState]::Minimized; Topmost = $false; Activations = 0 }
    $window | Add-Member -MemberType ScriptMethod -Name Activate -Value { $this.Activations++; $true }
    function Show-AppInfoDialog([string] $pkg) {
        $script:Shown.Add($pkg)
        # A request arriving while the dialog is open waits for it to close.
        [IO.File]::WriteAllText($script:PanelRequestFile, "com.example.nested`n")
        Invoke-PanelRequest
        Assert-Request (@(Get-ChildItem -LiteralPath $script:PanelRequestsDir -Filter '*.request').Count -eq 1) 'Another package lost its queued request'
        Get-ChildItem -LiteralPath $script:PanelRequestsDir -Filter '*.request' | Remove-Item
    }
    Invoke-PanelRequest
    Assert-Request ($window.Activations -eq 0) 'The panel came to the front with nothing to show'
    [IO.File]::WriteAllText($script:PanelRequestFile, "com.example.app`n")
    Invoke-PanelRequest
    Assert-Request ($script:Shown.Count -eq 1 -and $script:Shown[0] -eq 'com.example.app') 'The app details were not shown'
    Assert-Request ($window.WindowState -eq [System.Windows.WindowState]::Normal -and $window.Activations -eq 1) 'The panel was not brought to the front'
    Assert-Request (-not $script:ShowingAppInfo) 'The dialog guard was left set'

    foreach ($bad in '', '../../evil', 'com.example.app extra', 'single', '1com.example') {
        [IO.File]::WriteAllText($script:PanelRequestFile, "$bad`n")
        Invoke-PanelRequest
    }
    Assert-Request ($script:Shown.Count -eq 1) 'A request that is not a package name reached the dialog'
    Assert-Request ($window.Activations -eq 6) 'An empty or invalid request did not still raise the panel'

    # Busy: the request waits, but a second start's signal still raises the panel.
    $script:IsBusy = $true
    [IO.File]::WriteAllText($script:PanelRequestFile, "com.example.app`n")
    Invoke-PanelRequest
    Assert-Request (Test-Path -LiteralPath $script:PanelRequestFile) 'A busy panel took a request'
    Assert-Request ($window.Activations -eq 6) 'A busy panel came to the front for a request it keeps'
    $null = $script:PanelShowEvent.Set()
    Invoke-PanelRequest
    Assert-Request ($window.Activations -eq 7 -and $script:Shown.Count -eq 1) 'A second start did not raise a busy panel'
    Remove-Item -LiteralPath $script:PanelRequestFile -ErrorAction SilentlyContinue
    $script:IsBusy = $false

    # Result tokens (SettingsResults.java): one file per waiting request, holding its package
    # (ControlCenter.java). The details take their package's tokens and report them when they
    # close; a request for the app already shown joins it instead of reopening the details.
    $script:PanelResultsDir = Join-Path $root 'data/system/control-panel-results'
    New-Item -ItemType Directory -Force $script:PanelResultsDir | Out-Null
    function Write-Token([string] $token, [string] $pkg) { [IO.File]::WriteAllText((Join-Path $script:PanelResultsDir $token), "$pkg`n") }
    $script:Reported = [System.Collections.Generic.List[string]]::new()
    function Send-PanelSettingsClosed([string[]] $tokens) { $script:Reported.Add((@($tokens | Sort-Object) -join ',')) }
    $first = '0123456789abcdef0123456789abcdef'; $second = 'fedcba9876543210fedcba9876543210'; $third = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
    function Show-AppInfoDialog([string] $pkg) {
        $script:Shown.Add($pkg)
        Assert-Request ($script:ShowingPackage -eq $pkg -and $script:Reported.Count -eq 0) 'Nothing is reported while the details are open'
        Assert-Request ((Test-Path -LiteralPath $script:PanelShowingFile) -and [IO.File]::ReadAllText($script:PanelShowingFile) -eq $pkg) 'The shown package is recorded for the device'
        # The same app again, from a second request: taken into these details.
        Write-Token $second 'com.example.app'
        [IO.File]::WriteAllText($script:PanelRequestFile, "com.example.app`n")
        Invoke-PanelRequest
        Assert-Request (-not (Test-Path -LiteralPath $script:PanelRequestFile)) 'A request for the app shown was not taken'
        # Another app waits for the details to close; its token stays for its own details.
        Write-Token $third 'com.example.other'
        [IO.File]::WriteAllText($script:PanelRequestFile, "com.example.other`n")
        Invoke-PanelRequest
        Assert-Request (@(Get-ChildItem -LiteralPath $script:PanelRequestsDir -Filter '*.request').Count -eq 1) 'A request for another app was not retained in the queue'
        Assert-Request (Test-Path -LiteralPath (Join-Path $script:PanelResultsDir $third)) 'Another app''s token was taken'
    }
    $shownBefore = $script:Shown.Count
    Write-Token $first 'com.example.app'
    [IO.File]::WriteAllText($script:PanelRequestFile, "com.example.app`n")
    Invoke-PanelRequest
    Assert-Request ($script:Shown.Count -eq $shownBefore + 1) 'The same app was shown twice'
    $both = (@($first, $second) | Sort-Object) -join ','
    Assert-Request ($script:Reported.Count -eq 1 -and $script:Reported[0] -eq $both) "Both requests are answered as the details close: $($script:Reported -join ' / ')"
    Assert-Request (-not $script:ShowingAppInfo -and $null -eq $script:ShowingPackage -and $script:ShowingTokens.Count -eq 0) 'The shown state was left set'
    Assert-Request (-not (Test-Path -LiteralPath $script:PanelShowingFile)) 'The shown-package record outlived the details'
    # The waiting request for the other app opens next, and is answered in turn.
    function Show-AppInfoDialog([string] $pkg) { $script:Shown.Add($pkg) }
    Invoke-PanelRequest
    Assert-Request ($script:Shown[-1] -eq 'com.example.other' -and $script:Reported[-1] -eq $third) 'The waiting request was not shown and answered'
    Assert-Request (@(Get-ChildItem -LiteralPath $script:PanelResultsDir).Count -eq 0) 'Answered tokens were left behind'
    # A request with no waiting result (startActivity) reports nothing; the old tab form is not a package.
    $reports = $script:Reported.Count
    [IO.File]::WriteAllText($script:PanelRequestFile, "com.example.app`n")
    Invoke-PanelRequest
    Assert-Request ($script:Reported.Count -eq $reports) 'A request without a token reported something'
    [IO.File]::WriteAllText($script:PanelRequestFile, "com.example.app`t$first`n")
    $shownCount = $script:Shown.Count
    Invoke-PanelRequest
    Assert-Request ($script:Shown.Count -eq $shownCount -and $script:Reported.Count -eq $reports) 'A malformed request reached the details'
    # Immutable queue ownership: publish B exactly after the consumer selected A but
    # before its rename. The former single-file peek/take path silently consumed B here.
    $script:QueueSequence = 0L
    function Write-QueuedRequest([string] $pkg, [string] $token) {
        $script:QueueSequence++
        $name = '{0:D19}-{1}.request' -f $script:QueueSequence, $(if ($token) { $token } else { 'host-' + [guid]::NewGuid().ToString('N') })
        $path = Join-Path $script:PanelRequestsDir $name
        [IO.File]::WriteAllText($path, "$pkg`n$token`n")
        return $path
    }
    $raceA = '11111111111111111111111111111111'
    $raceB = '22222222222222222222222222222222'
    $raceC = '33333333333333333333333333333333'
    $script:ShowingAppInfo = $true
    $script:ShowingPackage = 'com.example.a'
    $script:ShowingTokens.Clear()
    $script:InjectBeforeClaim = Write-QueuedRequest 'com.example.a' $raceA
    function Move-Item {
        param([string] $LiteralPath, [string] $Destination, [switch] $Force, $ErrorAction)
        if ($LiteralPath -ceq $script:InjectBeforeClaim) {
            $script:InjectBeforeClaim = ''
            $null = Write-QueuedRequest 'com.example.b' $raceB
            $null = Write-QueuedRequest 'com.example.c' $raceC
        }
        Microsoft.PowerShell.Management\Move-Item -LiteralPath $LiteralPath -Destination $Destination -Force:$Force -ErrorAction Stop
    }
    try { Invoke-PanelRequest } finally { Remove-Item Function:Move-Item }
    Assert-Request ($script:ShowingTokens.Count -eq 1 -and $script:ShowingTokens[0] -eq $raceA) 'The current dialog claimed another package token'
    Assert-Request (@(Get-ChildItem -LiteralPath $script:PanelRequestsDir -Filter '*.request').Count -eq 2) 'A competing publisher lost a request'
    $script:ShowingAppInfo = $false
    $script:ShowingPackage = $null
    $before = $script:Reported.Count
    Invoke-PanelRequest
    Invoke-PanelRequest
    Assert-Request ($script:Shown[-2] -ceq 'com.example.b' -and $script:Shown[-1] -ceq 'com.example.c') 'Competing requests were not displayed in queue order'
    Assert-Request ($script:Reported.Count -eq $before + 2 -and $script:Reported[-2] -eq $raceB -and $script:Reported[-1] -eq $raceC) 'Each competing visit must report its own token exactly once'
    Invoke-PanelRequest
    Assert-Request ($script:Reported.Count -eq $before + 2) 'A claimed request was displayed or answered twice'

    # Tokenless and main-panel requests are complete queue records too.
    $null = Write-QueuedRequest 'com.example.plain' ''
    $null = Write-QueuedRequest '' ''
    Invoke-PanelRequest
    Assert-Request ($script:Shown[-1] -eq 'com.example.plain' -and $script:Reported.Count -eq $before + 2) 'A tokenless app request was lost or invented a result'
    $activations = $window.Activations
    Invoke-PanelRequest
    Assert-Request ($window.Activations -eq $activations + 1) 'The main-panel request was lost'
    Assert-Request (@(Get-ChildItem -LiteralPath $script:PanelRequestsDir).Count -eq 0) 'Claimed queue or temporary files were left behind'

    # Two panel processes around Reopen may inspect the same record. Only one owns it.
    $script:InjectBeforeClaim = Write-QueuedRequest 'com.example.a' $raceA
    $script:NestedClaim = $null
    function Move-Item {
        param([string] $LiteralPath, [string] $Destination, [switch] $Force, $ErrorAction)
        if ($LiteralPath -ceq $script:InjectBeforeClaim) {
            $script:InjectBeforeClaim = ''
            $script:NestedClaim = Receive-PanelRequest
        }
        Microsoft.PowerShell.Management\Move-Item -LiteralPath $LiteralPath -Destination $Destination -Force:$Force -ErrorAction Stop
    }
    try { $outerClaim = Receive-PanelRequest } finally { Remove-Item Function:Move-Item }
    Assert-Request ($null -eq $outerClaim -and $script:NestedClaim.Tokens.Count -eq 1 -and $script:NestedClaim.Tokens[0] -eq $raceA) 'Two consumers claimed the same request'

    # A producer refilling tokenless requests cannot monopolize the WPF dispatcher.
    $script:ShowingAppInfo = $true
    $script:ShowingPackage = 'com.example.flood'
    $script:FloodClaims = 0
    $null = Write-QueuedRequest 'com.example.flood' ''
    function Move-Item {
        param([string] $LiteralPath, [string] $Destination, [switch] $Force, $ErrorAction)
        Microsoft.PowerShell.Management\Move-Item -LiteralPath $LiteralPath -Destination $Destination -Force:$Force -ErrorAction Stop
        if ($LiteralPath.EndsWith('.request')) {
            $script:FloodClaims++
            if ($script:FloodClaims -le 64) { $null = Write-QueuedRequest 'com.example.flood' '' }
        }
    }
    try { Invoke-PanelRequest } finally { Remove-Item Function:Move-Item }
    Assert-Request ($script:FloodClaims -gt 0 -and $script:FloodClaims -lt 64) 'A continuously refilled queue monopolized one dispatcher tick'
    Assert-Request (@(Get-ChildItem -LiteralPath $script:PanelRequestsDir -Filter '*.request').Count -eq 1) 'The next tick lost its pending request'
    $script:ShowingAppInfo = $false
    $script:ShowingPackage = $null

} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
"PASS: control panel App info requests ($script:Checks checks)"
