param(
    [string] $PanelPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-control-panel.ps1'),
    [string] $UpdatePath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-update.ps1')
)

$ErrorActionPreference = 'Stop'
$PanelPath = [IO.Path]::GetFullPath($PanelPath)
$UpdatePath = [IO.Path]::GetFullPath($UpdatePath)
$packageDirectory = Split-Path -Parent $PanelPath
. $UpdatePath

# Run the actual state machine, renderer and asynchronous runspace plumbing.
# Only network responses, installer execution and confirmation are fixtures.
$tokens = $null; $parseErrors = $null
$panelAst = [Management.Automation.Language.Parser]::ParseFile($PanelPath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
foreach ($function in $panelAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -match '^[A-Za-z]+-Panel'
}, $false)) {
    . ([scriptblock]::Create($function.Extent.Text))
}
$uiAst = [Management.Automation.Language.Parser]::ParseFile((Join-Path $packageDirectory 'gesher-ui.ps1'), [ref]$tokens, [ref]$parseErrors)
$textFunction = $uiAst.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-GesherText' }, $false)
. ([scriptblock]::Create($textFunction.Extent.Text))
$script:GesherUiCatalog = Get-Content -LiteralPath (Join-Path $packageDirectory 'ui-messages.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$script:GesherUiLanguage = 'en'
$script:Checks = 0
$script:CaseNumber = 0
$script:TestRoot = Join-Path ([IO.Path]::GetTempPath()) ('gesher-panel-update-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $script:TestRoot

function Assert-Update([bool] $condition, [string] $message) {
    if (-not $condition) { throw $message }
    $script:Checks++
}
function Set-Busy([bool] $busy, [string] $message = '') { $script:IsBusy = $busy }
function Refresh-Dashboard { $script:DashboardRefreshes++ }
function Confirm-PanelUpdateInstallation { $script:Confirmations++; return $script:ApproveInstall }
function New-FixtureControl {
    [pscustomobject]@{ Text = ''; ToolTip = ''; Content = ''; Visibility = 'Visible'; IsEnabled = $true; IsChecked = $false }
}
function Save-FixtureConfiguration {
    [IO.File]::WriteAllText((Join-Path $here 'fixture.json'), ($script:Fixture | ConvertTo-Json), [Text.UTF8Encoding]::new($true))
}
function Drain-Worker {
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ($script:UpdateState.Worker) {
        Complete-PanelUpdateWorker
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Update worker did not finish within 15 seconds' }
        if ($script:UpdateState.Worker) { Start-Sleep -Milliseconds 10 }
    }
}
function Wait-WorkerResult {
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while (-not $script:UpdateState.Worker.Handle.IsCompleted) {
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Runspace result did not finish within 15 seconds' }
        Start-Sleep -Milliseconds 10
    }
}
function Drain-Installer {
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ($script:UpdateState.Worker -or $script:UpdateState.Installer) {
        Invoke-PanelUpdateTick
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Fixture installer did not finish within 15 seconds' }
        if ($script:UpdateState.Worker -or $script:UpdateState.Installer) { Start-Sleep -Milliseconds 10 }
    }
}
function Reset-PanelUpdateFixture {
    if ($script:UpdateState -and ($script:UpdateState.Worker -or $script:UpdateState.Installer)) { throw 'Previous fixture still owns a worker/process' }
    $script:CaseNumber++
    $script:here = Join-Path $script:TestRoot ([string]$script:CaseNumber)
    $null = New-Item -ItemType Directory -Path $here
    $script:UpdateDirectory = Join-Path $here 'updates'
    $script:IsBusy = $false; $script:ApproveInstall = $false; $script:Confirmations = 0; $script:DashboardRefreshes = 0
    $script:Fixture = @{
        CheckStatus = 'Available'; CheckDelay = 100; CheckFailure = $false
        DownloadDelay = 100; DownloadFailure = $false
        ValidationDelay = 100; InstallFailure = $false; InstallDelay = 500
        InstallExitCode = 0; InstalledVersion = '0.2.0'
    }
    Save-FixtureConfiguration
    [IO.File]::WriteAllText((Join-Path $here 'gesher-version.ini'), "[Gesher]`nVersion=0.1.0`n")
    $fixtureHelper = ". '" + $UpdatePath.Replace("'", "''") + "'`n" + @'
function Get-Fixture { Get-Content -LiteralPath (Join-Path $PSScriptRoot 'fixture.json') -Raw | ConvertFrom-Json }
function Wait-Fixture([int] $milliseconds, [Threading.CancellationToken] $token) {
    $elapsed = [Diagnostics.Stopwatch]::StartNew()
    do { $token.ThrowIfCancellationRequested(); Start-Sleep -Milliseconds 10 } while ($elapsed.ElapsedMilliseconds -lt $milliseconds)
    $token.ThrowIfCancellationRequested()
}
function Get-GesherUpdate($InstallDirectory, [Threading.CancellationToken] $CancellationToken) {
    $config = Get-Fixture
    Wait-Fixture $config.CheckDelay $CancellationToken
    if ($config.CheckFailure) { throw 'Fixture network failure' }
    $release = if ($config.CheckStatus -in 'Available', 'Current') {
        [pscustomobject]@{ Version = '0.2.0'; Tag = 'v0.2.0'; Url = 'https://github.com/gesher-runtime/gesher/releases/tag/v0.2.0' }
    } else { $null }
    [pscustomobject]@{ Status = $config.CheckStatus; CurrentVersion = (Get-GesherInstalledVersion $InstallDirectory); Release = $release }
}
function Receive-GesherUpdate($Release, $DestinationDirectory, [Threading.CancellationToken] $CancellationToken) {
    $config = Get-Fixture
    $directory = Join-Path $DestinationDirectory ([guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $directory -Force
    $path = Join-Path $directory 'fixture-installer.exe'
    try {
        [IO.File]::WriteAllText($path, 'fixture bytes; never executed')
        Wait-Fixture $config.DownloadDelay $CancellationToken
        if ($config.DownloadFailure) { throw 'Fixture download failure' }
        [pscustomobject]@{ Path = $path; Directory = $directory; Release = $Release }
    } catch {
        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $directory -Force -ErrorAction SilentlyContinue
        throw
    }
}
function Start-GesherUpdateInstaller($Download, $InstallDirectory, [Threading.CancellationToken] $CancellationToken) {
    $config = Get-Fixture
    Wait-Fixture $config.ValidationDelay $CancellationToken
    if ($config.InstallFailure) { throw 'Fixture process start failure' }
    $CancellationToken.ThrowIfCancellationRequested()
    [IO.File]::WriteAllText((Join-Path $InstallDirectory 'gesher-version.ini'), "[Gesher]`nVersion=$($config.InstalledVersion)`n")
    [IO.File]::WriteAllText((Join-Path $InstallDirectory 'setup-started'), 'approved fixture setup')
    $child = 'Start-Sleep -Milliseconds ' + [int]$config.InstallDelay + '; exit ' + [int]$config.InstallExitCode
    $exe = Join-Path $PSHOME 'powershell.exe'
    if (-not (Test-Path -LiteralPath $exe)) { $exe = Join-Path $PSHOME 'pwsh.exe' }
    if (-not (Test-Path -LiteralPath $exe)) { $exe = Join-Path $PSHOME 'pwsh' }
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $exe
    $start.Arguments = '-NoLogo -NoProfile -NonInteractive -EncodedCommand ' + [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($child))
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    [Diagnostics.Process]::Start($start)
}
'@
    [IO.File]::WriteAllText((Join-Path $here 'gesher-update.ps1'), $fixtureHelper, [Text.UTF8Encoding]::new($true))
    foreach ($name in 'updateVersionText', 'updateStatusText', 'automaticUpdateChecks', 'btnCheckUpdate', 'btnInstallUpdate', 'btnCancelUpdate', 'btnUpdateNotes', 'btnReopenPanel', 'updateProgress', 'btnToggleDevice', 'servicesCards', 'cameraCard', 'applicationsSection') {
        Set-Variable -Scope Script -Name $name -Value (New-FixtureControl)
    }
    $script:window = [pscustomobject]@{ AllowDrop = $true; IsActive = $true }
    $script:UpdateState = New-PanelUpdateState (Get-GesherInstalledVersion $here) (Get-GesherUpdatePreferences $script:UpdateDirectory)
    Update-PanelUpdateView
}
function Prepare-AvailableUpdate {
    Start-PanelUpdateCheck
    Drain-Worker
    Assert-Update ($script:UpdateState.Phase -eq 'Available') 'Fixture release was not available'
}
function Prepare-DownloadedUpdate {
    Prepare-AvailableUpdate
    Start-PanelUpdateDownload
    Drain-Worker
    Assert-Update ($script:UpdateState.Phase -eq 'Ready') 'Fixture download was not staged'
}

try {
    Reset-PanelUpdateFixture
    Start-PanelUpdateCheck -Automatic
    $worker = $script:UpdateState.Worker
    Assert-Update ($worker -and $script:UpdateState.Phase -eq 'Checking') 'Automatic check did not start'
    Assert-Update ([bool](Get-GesherUpdatePreferences $script:UpdateDirectory).LastCheckUtc) 'Attempt timestamp was not saved before the request'
    Assert-Update (-not $script:IsBusy -and $servicesCards.IsEnabled -and $applicationsSection.IsEnabled) 'Checking blocked application controls'
    Assert-Update (-not $btnCheckUpdate.IsEnabled -and $btnCancelUpdate.Visibility -eq 'Visible') 'Checking UI did not prevent duplicate work'
    Start-PanelUpdateCheck
    Start-PanelUpdateDownload
    Assert-Update ([object]::ReferenceEquals($worker, $script:UpdateState.Worker)) 'Repeated clicks replaced a live worker'
    Drain-Worker
    Assert-Update ($worker.Disposed -and $script:UpdateState.Phase -eq 'Available') 'Worker was not disposed after releasing its result'
    Assert-Update ($btnInstallUpdate.Visibility -eq 'Visible' -and $btnUpdateNotes.Visibility -eq 'Visible') 'Available update actions were not displayed'
    Start-PanelUpdateCheck -Automatic
    Assert-Update ($null -eq $script:UpdateState.Worker) 'Automatic check ignored its daily interval'
    Start-PanelUpdateCheck
    Assert-Update ($null -ne $script:UpdateState.Worker) 'Manual check did not bypass the interval'
    Request-PanelUpdateCancellation
    Drain-Worker

    Reset-PanelUpdateFixture
    Set-PanelAutomaticUpdateChecks $false
    Assert-Update (-not (Get-GesherUpdatePreferences $script:UpdateDirectory).AutomaticChecks) 'Disabling automatic checks was not persisted'
    Start-PanelUpdateCheck -Automatic
    Assert-Update ($null -eq $script:UpdateState.Worker) 'Disabled automatic checking made a request'
    Start-PanelUpdateCheck
    Assert-Update (-not $script:UpdateState.Preferences.AutomaticChecks) 'Manual checking re-enabled automatic checks'
    Drain-Worker

    Reset-PanelUpdateFixture
    $script:Fixture.CheckDelay = 1500; Save-FixtureConfiguration
    Start-PanelUpdateCheck -Automatic
    $worker = $script:UpdateState.Worker
    $elapsed = [Diagnostics.Stopwatch]::StartNew()
    Set-PanelAutomaticUpdateChecks $false
    Assert-Update ($elapsed.ElapsedMilliseconds -lt 750) 'Canceling a check blocked on the request'
    Assert-Update ($worker.CancelRequested -and $script:UpdateState.Phase -eq 'Cancelling') 'Disabling automatic checks did not cancel its active check'
    Drain-Worker
    Assert-Update ($worker.Disposed -and $script:UpdateState.StatusKey -eq 'cp_update_cancelled') 'Cancellation leaked a worker or became a failure'

    Reset-PanelUpdateFixture
    $script:Fixture.CheckFailure = $true; Save-FixtureConfiguration
    Start-PanelUpdateCheck -Automatic; Drain-Worker
    Assert-Update ($script:UpdateState.StatusKey -eq 'cp_update_check_failed' -and $btnCheckUpdate.IsEnabled) 'Failed check could not be retried'
    Start-PanelUpdateCheck -Automatic
    Assert-Update ($null -eq $script:UpdateState.Worker) 'A failed automatic check was retried without throttling'
    foreach ($status in 'Current', 'NoRelease', 'UnknownVersion') {
        Reset-PanelUpdateFixture
        $script:Fixture.CheckStatus = $status; Save-FixtureConfiguration
        Start-PanelUpdateCheck; Drain-Worker
        Assert-Update ($btnInstallUpdate.Visibility -eq 'Collapsed' -and $null -eq $script:UpdateState.Installer) "$status incorrectly offered installation"
    }

    Reset-PanelUpdateFixture
    Prepare-AvailableUpdate
    Start-PanelUpdateDownload
    Assert-Update (-not $script:IsBusy -and $applicationsSection.IsEnabled -and $window.AllowDrop) 'Download blocked application management'
    $script:IsBusy = $true
    Drain-Worker; Invoke-PanelUpdateTick
    Assert-Update ($script:UpdateState.Phase -eq 'Ready' -and $script:Confirmations -eq 0) 'Download interrupted another dashboard action with a prompt'
    $script:IsBusy = $false; $window.IsActive = $false; Invoke-PanelUpdateTick
    Assert-Update ($script:Confirmations -eq 0 -and $script:UpdateState.PromptPending) 'A nested modal dialog or inactive panel was interrupted by installation'
    $window.IsActive = $true; Invoke-PanelUpdateTick
    $download = $script:UpdateState.Download
    Assert-Update ($script:Confirmations -eq 1 -and $script:UpdateState.Phase -eq 'Ready' -and (Test-Path $download.Path)) 'Declining setup discarded the verified download'
    Invoke-PanelUpdateTick
    Assert-Update ($script:Confirmations -eq 1 -and -not (Test-Path (Join-Path $here 'setup-started'))) 'Declined setup prompted again or started an installer'
    Request-PanelUpdateCancellation
    Assert-Update (-not (Test-Path $download.Directory)) 'Discarding a ready download left its files behind'

    Reset-PanelUpdateFixture
    Prepare-AvailableUpdate
    Start-PanelUpdateDownload; Wait-WorkerResult
    $worker = $script:UpdateState.Worker
    Request-PanelUpdateCancellation; Drain-Worker; Invoke-PanelUpdateTick
    Assert-Update ($worker.Disposed -and $script:Confirmations -eq 0 -and $null -eq $script:UpdateState.Download) 'Late canceled result was offered for installation'
    Assert-Update (@(Get-ChildItem -LiteralPath (Join-Path $script:UpdateDirectory 'downloads') -Force).Count -eq 0) 'A completed download canceled before publication was not removed'

    Reset-PanelUpdateFixture
    Prepare-AvailableUpdate
    $script:Fixture.DownloadDelay = 1500; Save-FixtureConfiguration
    Start-PanelUpdateDownload
    $worker = $script:UpdateState.Worker
    Close-PanelUpdates; Complete-PanelUpdateWorker -Wait
    Assert-Update ($script:UpdateState.Closed -and $worker.Disposed -and $null -eq $script:UpdateState.Worker) 'Closing did not cancel and dispose the worker'
    Assert-Update (-not (Test-Path (Join-Path $here 'setup-started')) -and $script:Confirmations -eq 0) 'Closing the panel started an installer'

    Reset-PanelUpdateFixture
    Prepare-DownloadedUpdate
    $download = $script:UpdateState.Download
    $script:ApproveInstall = $true
    Start-PanelUpdateInstallation
    Assert-Update ($script:IsBusy -and -not $servicesCards.IsEnabled -and -not $cameraCard.IsEnabled -and -not $applicationsSection.IsEnabled -and -not $window.AllowDrop) 'Setup did not guard conflicting dashboard actions'
    Assert-Update ($script:UpdateState.Worker.Operation -eq 'Install' -and $script:UpdateState.StatusKey -ne 'cp_update_installed_success') 'Installer validation was synchronous or prematurely claimed success'
    Drain-Worker
    Assert-Update ($script:UpdateState.Installer -is [Diagnostics.Process] -and $script:IsBusy) 'Installer process was not retained for exit monitoring'
    Drain-Installer
    Assert-Update ($script:UpdateState.Phase -eq 'Completed' -and $script:UpdateState.CurrentVersion -eq '0.2.0') 'Successful setup was not verified against the installed version'
    Assert-Update (-not $script:IsBusy -and $applicationsSection.IsEnabled -and $btnReopenPanel.Visibility -eq 'Visible') 'Successful setup did not release the dashboard or offer reopening'
    Assert-Update (-not (Test-Path $download.Directory) -and $script:DashboardRefreshes -eq 1) 'Successful setup did not clean staging and refresh the dashboard'

    foreach ($scenario in @(
        @{ Code = 0; Version = '0.1.0'; Expected = 'cp_update_unverified' },
        @{ Code = 2; Version = '0.1.0'; Expected = 'cp_update_setup_cancelled' },
        @{ Code = 5; Version = '0.1.0'; Expected = 'cp_update_setup_cancelled' },
        @{ Code = 1; Version = '0.1.0'; Expected = 'cp_update_install_failed' }
    )) {
        Reset-PanelUpdateFixture; Prepare-DownloadedUpdate
        $script:Fixture.InstallExitCode = $scenario.Code; $script:Fixture.InstalledVersion = $scenario.Version; Save-FixtureConfiguration
        $script:ApproveInstall = $true
        Start-PanelUpdateInstallation; Drain-Installer
        Assert-Update ($script:UpdateState.Phase -eq 'Available' -and $script:UpdateState.StatusKey -eq $scenario.Expected) "Incorrect setup outcome for exit $($scenario.Code) and version $($scenario.Version)"
        Assert-Update (-not $script:IsBusy -and $applicationsSection.IsEnabled -and $null -eq $script:UpdateState.Download) 'Failed/canceled setup leaked busy state or staged data'
    }

    Reset-PanelUpdateFixture; Prepare-DownloadedUpdate
    $script:Fixture.InstallFailure = $true; Save-FixtureConfiguration
    $script:ApproveInstall = $true
    Start-PanelUpdateInstallation; Drain-Worker
    Assert-Update ($script:UpdateState.Phase -eq 'Ready' -and -not $script:IsBusy -and (Test-Path $script:UpdateState.Download.Path)) 'Process-start failure prevented retry or left the dashboard busy'
    Request-PanelUpdateCancellation

    Reset-PanelUpdateFixture; Prepare-DownloadedUpdate
    $script:Fixture.ValidationDelay = 1500; Save-FixtureConfiguration
    $script:ApproveInstall = $true
    Start-PanelUpdateInstallation; Close-PanelUpdates; Complete-PanelUpdateWorker -Wait
    Assert-Update (-not (Test-Path (Join-Path $here 'setup-started')) -and $null -eq $script:UpdateState.Download) 'Closing during validation allowed a late installer launch or retained its download'

    Reset-PanelUpdateFixture; Prepare-DownloadedUpdate
    $download = $script:UpdateState.Download
    $sentinel = Join-Path $download.Directory 'keep.txt'
    [IO.File]::WriteAllText($sentinel, 'another file')
    Request-PanelUpdateCancellation
    Assert-Update ((Test-Path $sentinel) -and -not (Test-Path $download.Path)) 'Staging cleanup deleted an unrelated file or retained its installer'

    Reset-PanelUpdateFixture
    $oldPreferences = $script:UpdateState.Preferences
    $script:UpdateDirectory = Join-Path $here 'not-a-directory'
    [IO.File]::WriteAllText($script:UpdateDirectory, 'fixture')
    Set-PanelAutomaticUpdateChecks $false
    Assert-Update ($script:UpdateState.Preferences.AutomaticChecks -eq $oldPreferences.AutomaticChecks -and $automaticUpdateChecks.IsChecked) 'A failed preference save changed the effective checkbox choice'
    Assert-Update ($script:UpdateState.StatusKey -eq 'cp_update_preferences_failed') 'Preference write failure was hidden'
    Start-PanelUpdateCheck
    Assert-Update ($null -eq $script:UpdateState.Worker) 'A check started without recording its attempt'

    # Construct the real WPF XAML in both languages. On short displays the outer
    # viewport must let the actual application buttons scroll into view.
    if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
        Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase
        $xamlAssignment = @($panelAst.FindAll({
            param($node)
            $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -eq '[xml]$xaml'
        }, $false))
        Assert-Update ($xamlAssignment.Count -eq 1) 'Could not locate production XAML'
        foreach ($language in 'en', 'he') {
            foreach ($size in @(@(920, 820), @(800, 600))) {
                Reset-PanelUpdateFixture
                $script:GesherUiLanguage = $language
                $flowDirection = if ($language -eq 'he') { 'RightToLeft' } else { 'LeftToRight' }
                . ([scriptblock]::Create($xamlAssignment[0].Extent.Text))
                $script:window = [Windows.Markup.XamlReader]::Load([Xml.XmlNodeReader]::new($xaml))
                try {
                    $window.ShowInTaskbar = $false; $window.WindowStartupLocation = 'Manual'; $window.Left = -12000
                    $window.Width = $size[0]
                    $window.MaxHeight = [Windows.SystemParameters]::WorkArea.Height
                    $window.MinHeight = [Math]::Min($window.MinHeight, $window.MaxHeight)
                    $window.Height = [Math]::Min($size[1], $window.MaxHeight)
                    $names = @{
                        updateVersionText = 'UpdateVersionText'; updateStatusText = 'UpdateStatusText'; automaticUpdateChecks = 'AutomaticUpdateChecks'
                        btnCheckUpdate = 'BtnCheckUpdate'; btnInstallUpdate = 'BtnInstallUpdate'; btnCancelUpdate = 'BtnCancelUpdate'
                        btnUpdateNotes = 'BtnUpdateNotes'; btnReopenPanel = 'BtnReopenPanel'; updateProgress = 'UpdateProgress'
                        btnToggleDevice = 'BtnToggleDevice'; servicesCards = 'ServicesCards'; cameraCard = 'CameraCard'; applicationsSection = 'ApplicationsSection'
                        panelScroll = 'PanelScroll'; panelLayout = 'PanelLayout'
                    }
                    foreach ($entry in $names.GetEnumerator()) { Set-Variable -Scope Script -Name $entry.Key -Value ($window.FindName($entry.Value)) }
                    foreach ($name in 'BtnEnableMicrog', 'BtnInstallStore') { $window.FindName($name).Visibility = 'Collapsed' }
                    $script:UpdateState.Phase = 'Available'
                    $script:UpdateState.Release = [pscustomobject]@{ Version = '0.2.0' }
                    Set-PanelUpdateStatus 'cp_update_available' @('0.2.0')
                    Update-PanelUpdateView
                    $window.FindName('AppsList').ItemsSource = @([pscustomobject]@{ Label = 'Fixture app'; Package = 'fixture.app'; IconImage = $null })
                    $iconPath = Join-Path $here 'window-icon.ico'
                    [IO.File]::Copy((Join-Path $packageDirectory 'gesher.ico'), $iconPath)
                    $window.Icon = Get-PanelWindowIcon $iconPath
                    $exclusive = [IO.File]::Open($iconPath, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
                    $exclusive.Dispose()
                    Assert-Update ($window.Icon.IsFrozen) 'Window icon retained an installer-blocking stream or mutable image'
                    $window.Show(); $window.UpdateLayout(); Update-PanelWindowLayout; $window.UpdateLayout()
                    Assert-Update ([string]$window.FlowDirection -eq $flowDirection) 'WPF window direction does not follow the UI language'
                    Assert-Update ($applicationsSection.ActualHeight -ge 120) "Application area was squeezed out at $language $($size -join 'x')"
                    $appList = $window.FindName('AppsList')
                    Assert-Update ($appList.Template.FindName('AppsViewport', $appList).ViewportHeight -ge 36) 'The inner application viewport clipped every app row'
                    foreach ($button in $btnCheckUpdate, $btnInstallUpdate, $automaticUpdateChecks) {
                        $bounds = $button.TransformToAncestor($panelScroll).TransformBounds([Windows.Rect]::new(0, 0, $button.ActualWidth, $button.ActualHeight))
                        Assert-Update ($button.ActualWidth -gt 0 -and $bounds.Left -ge -1 -and $bounds.Right -le $panelScroll.ViewportWidth + 1) 'Update controls overflowed the viewport horizontally'
                    }
                    $panelScroll.ScrollToEnd(); $window.UpdateLayout()
                    $window.Dispatcher.Invoke([Action]{}, [Windows.Threading.DispatcherPriority]::Render)
                    $appList = $window.FindName('AppsList')
                    $position = $appList.TransformToAncestor($panelScroll).Transform([Windows.Point]::new(0, 0))
                    Assert-Update ($appList.ActualHeight -gt 30 -and $position.Y -ge -1 -and $position.Y + $appList.ActualHeight -le $panelScroll.ViewportHeight + 1) 'Application controls could not be scrolled into the visible viewport'
                } finally { $window.Close() }
            }
        }
    } else { 'SKIP: native WPF layout requires Windows (covered by the Windows CI job)' }
    "PASS: control panel update contracts ($script:Checks checks)"
} finally {
    if ($script:UpdateState) {
        Close-PanelUpdates
        Complete-PanelUpdateWorker -Wait
        if ($script:UpdateState.Installer) {
            if (-not $script:UpdateState.Installer.HasExited) { $script:UpdateState.Installer.Kill(); $null = $script:UpdateState.Installer.WaitForExit(5000) }
            $script:UpdateState.Installer.Dispose()
        }
    }
    Remove-Item -LiteralPath $script:TestRoot -Recurse -Force -ErrorAction SilentlyContinue
}
