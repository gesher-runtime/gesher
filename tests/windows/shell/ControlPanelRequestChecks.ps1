param([string] $PanelPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-control-panel.ps1'))

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName WindowsBase, PresentationFramework
# An app's "App info" (ADR 0087): the system server writes a package to
# data/system/control-panel-request and opens the Control Center. Execute the production
# request functions without a WPF window; the window and the dialog are stand-ins here.
$tokens = $null; $parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    [IO.Path]::GetFullPath($PanelPath), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
foreach ($name in 'Read-PanelRequest', 'Get-AppInstallTimes', 'Invoke-PanelRequest', 'ConvertFrom-PanelRequest') {
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

# One panel: run the production startup lines (single-panel decision) in child processes,
# as a second start does, with the first panel's mutex held here.
$text = [IO.File]::ReadAllText([IO.Path]::GetFullPath($PanelPath))
$start = $text.IndexOf('$script:PanelReopen = ')
$end = $text.IndexOf("`n}", $text.IndexOf('if (-not $script:PanelInstanceCreated -and -not $script:PanelReopen)'))
if ($start -lt 0 -or $end -lt 0) { throw 'The single-panel startup block was not found' }
$startup = $text.Substring($start, $end + 2 - $start)
$suffix = [guid]::NewGuid().ToString('N')
$startup = $startup.Replace('Local\Gesher.ControlPanel', "Local\Gesher.ControlPanelCheck$suffix")
$probe = Join-Path ([IO.Path]::GetTempPath()) "gesher-panel-start-$suffix.ps1"
[IO.File]::WriteAllText($probe, $startup + "`nexit 7`n")
$created = $false
$mutex = [System.Threading.Mutex]::new($false, "Local\Gesher.ControlPanelCheck$suffix", [ref] $created)
$show = [System.Threading.EventWaitHandle]::new($false, [System.Threading.EventResetMode]::AutoReset, "Local\Gesher.ControlPanelCheck$suffix.Show")
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

$root = Join-Path ([IO.Path]::GetTempPath()) ("gesher-panel-request-" + [guid]::NewGuid())
New-Item -ItemType Directory -Force (Join-Path $root 'data/system') | Out-Null
try {
    $script:PanelRequestFile = Join-Path $root 'data/system/control-panel-request'

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
        Assert-Request (Test-Path -LiteralPath $script:PanelRequestFile) 'A request was taken while a dialog was open'
        Remove-Item -LiteralPath $script:PanelRequestFile
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

    # Result tokens (SettingsResults.java): reported when the details close; a request for
    # the app already shown joins it instead of reopening the details afterwards.
    $script:Reported = [System.Collections.Generic.List[string]]::new()
    function Send-PanelSettingsClosed([string[]] $tokens) { $script:Reported.Add(($tokens -join ',')) }
    $first = '0123456789abcdef0123456789abcdef'; $second = 'fedcba9876543210fedcba9876543210'; $third = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
    function Show-AppInfoDialog([string] $pkg) {
        $script:Shown.Add($pkg)
        Assert-Request ($script:ShowingPackage -eq $pkg -and $script:Reported.Count -eq 0) 'Nothing is reported while the details are open'
        # The same app again, from a second request: taken into these details.
        [IO.File]::WriteAllText($script:PanelRequestFile, "com.example.app`t$second`n")
        Invoke-PanelRequest
        Assert-Request (-not (Test-Path -LiteralPath $script:PanelRequestFile)) 'A request for the app shown was not taken'
        # Another app waits for the details to close.
        [IO.File]::WriteAllText($script:PanelRequestFile, "com.example.other`t$third`n")
        Invoke-PanelRequest
        Assert-Request (Test-Path -LiteralPath $script:PanelRequestFile) 'A request for another app was taken while a dialog was open'
    }
    $shownBefore = $script:Shown.Count
    [IO.File]::WriteAllText($script:PanelRequestFile, "com.example.app`t$first`n")
    Invoke-PanelRequest
    Assert-Request ($script:Shown.Count -eq $shownBefore + 1) 'The same app was shown twice'
    Assert-Request ($script:Reported.Count -eq 1 -and $script:Reported[0] -eq "$first,$second") "Both requests are answered as the details close: $($script:Reported -join ' / ')"
    Assert-Request (-not $script:ShowingAppInfo -and $null -eq $script:ShowingPackage -and $script:ShowingTokens.Count -eq 0) 'The shown state was left set'
    # The waiting request for the other app opens next, and is answered in turn.
    function Show-AppInfoDialog([string] $pkg) { $script:Shown.Add($pkg) }
    Invoke-PanelRequest
    Assert-Request ($script:Shown[-1] -eq 'com.example.other' -and $script:Reported[-1] -eq $third) 'The waiting request was not shown and answered'
    # A request with no token (startActivity) reports nothing; a malformed token is not a package.
    $reports = $script:Reported.Count
    [IO.File]::WriteAllText($script:PanelRequestFile, "com.example.app`n")
    Invoke-PanelRequest
    Assert-Request ($script:Reported.Count -eq $reports) 'A request without a token reported something'
    [IO.File]::WriteAllText($script:PanelRequestFile, "com.example.app`tNOT-A-TOKEN`n")
    $shownCount = $script:Shown.Count
    Invoke-PanelRequest
    Assert-Request ($script:Shown.Count -eq $shownCount -and $script:Reported.Count -eq $reports) 'A malformed token reached the details'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
"PASS: control panel App info requests ($script:Checks checks)"
