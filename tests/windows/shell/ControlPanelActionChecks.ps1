param([string] $PanelPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-control-panel.ps1'))

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile([IO.Path]::GetFullPath($PanelPath), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
foreach ($name in 'Set-Busy','Complete-DashboardAction','Install-PanelApk','New-PanelActionWorker',
        'Start-PanelAction','Complete-PanelAction','Test-PanelActionClose') {
    $definition = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $false))
    if ($definition.Count -ne 1) { throw "Expected one production function: $name" }
    $functionSource = $definition[0].Extent.Text
    if ($name -eq 'New-PanelActionWorker') {
        # Reject compiler invocation in the real owner and child paths. The
        # surrounding UI/IPC/native process code is the production implementation.
        $functionSource = $functionSource.Replace("`$ErrorActionPreference = 'Stop'", "`$ErrorActionPreference = 'Stop'`nfunction Add-Type { throw 'Action helper unexpectedly invoked a compiler' }")
    }
    . ([scriptblock]::Create($functionSource))
}
# The native functions are nested inside the production owner. Extract them
# unchanged for ABI inspection and a direct, warmed native failure census.
foreach ($name in 'Initialize-PanelActionNative', 'Invoke-PanelActionNative') {
    $definition = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $true))
    if ($definition.Count -ne 1) { throw "Expected one production native function: $name" }
    . ([scriptblock]::Create($definition[0].Extent.Text))
}
$script:Checks = 0
function Assert-Action([bool] $value, [string] $message) {
    if (-not $value) { throw $message }
    $script:Checks++
}
function Show-ModernMessageDialog([string] $Title, [string] $Message, [bool] $IsError) {
    $script:Messages.Add([PSCustomObject]@{ Message = $Message; Thread = [Threading.Thread]::CurrentThread.ManagedThreadId })
}
function Refresh-Dashboard {
    $script:Refreshes++
    if ($script:FailRefresh) { throw 'fixture refresh failed' }
    Assert-Action ([Threading.Thread]::CurrentThread.ManagedThreadId -eq $script:UiThread) 'Refresh crossed the dispatcher thread'
}
function Reset-Action {
    $script:ActionState = @{ Worker = $null; Completing = $false; Closed = $false }
    $script:UpdateState = @{ Phase = 'Idle' }
    $script:IsBusy = $false
    $script:Messages = [Collections.Generic.List[object]]::new()
    $script:Refreshes = 0; $script:FailRefresh = $false
    $script:Fixture = @{ Delay = 0; Exit = 0; MicrogExit = 0; DownloadFails = $false; NativeLogs = $true; LargeOutput = $false; ExitHost = $false }
    Save-ActionFixture
    [IO.File]::WriteAllText((Join-Path $here 'calls.log'), '')
    Set-Busy $false
}
function Save-ActionFixture {
    [IO.File]::WriteAllText((Join-Path $here 'fixture.json'), ($script:Fixture | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
}
function Read-ActionCalls {
    ,@(Get-Content -LiteralPath (Join-Path $here 'calls.log') -Encoding UTF8 | Where-Object { $_ } | ForEach-Object { $_ | ConvertFrom-Json })
}
function Drain-Action {
    # Real WPF dispatcher ticks while the real background runspace is blocked.
    # No sleeps, worker waits or EndInvoke on an incomplete handle occur here.
    $script:PumpTicks = 0; $script:PumpFailure = $null
    $script:PumpDeadline = [DateTime]::UtcNow.AddSeconds(15)
    $script:PumpFrame = [Windows.Threading.DispatcherFrame]::new()
    $timer = [Windows.Threading.DispatcherTimer]::new()
    $timer.Interval = [TimeSpan]::FromMilliseconds(10)
    $timer.Add_Tick({
        $script:PumpTicks++
        try {
            Complete-PanelAction
            if ([DateTime]::UtcNow -gt $script:PumpDeadline) { throw 'Dashboard action exceeded fixture deadline' }
        } catch { $script:PumpFailure = $_; $script:PumpFrame.Continue = $false }
        if (-not $script:ActionState.Worker) { $script:PumpFrame.Continue = $false }
    })
    try { $timer.Start(); [Windows.Threading.Dispatcher]::PushFrame($script:PumpFrame) }
    finally { $timer.Stop() }
    if ($script:PumpFailure) { throw $script:PumpFailure }
}

$scratchRoot = [IO.Path]::GetFullPath((Join-Path $repo 'out')) + [IO.Path]::DirectorySeparatorChar
# Exercise native process/script quoting independently of the request's paths.
# Codepoints preserve real Unicode under Windows PowerShell's source decoding.
$here = Join-Path $scratchRoot ('panel-actions- space & '' ' + [char]0x05e9 + [char]0x05dc + [char]0x05d5 + [char]0x05dd + [char]0x4e2d + '-' + [guid]::NewGuid().ToString('N'))
$null = [IO.Directory]::CreateDirectory($here)
$savedEnvironment = @{}
foreach ($name in 'TMP','TEMP','GESHER_LAUNCH_ERROR_FILE','GESHER_LOCALES','GESHER_PANEL_UI_LANGUAGE') {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$script:UiThread = [Threading.Thread]::CurrentThread.ManagedThreadId
$window = [Windows.Window]::new()
$activityBar = [Windows.Controls.ProgressBar]::new()
$statusMessage = [Windows.Controls.TextBlock]::new()
$btnToggleDevice = [Windows.Controls.Button]::new()
$btnInstallUpdate = [Windows.Controls.Button]::new()
$btnReopenPanel = [Windows.Controls.Button]::new()
$servicesCards = [Windows.Controls.StackPanel]::new()
$cameraCard = [Windows.Controls.Border]::new()
$applicationsSection = [Windows.Controls.GroupBox]::new()
try {
    # The real panel loads its UI helper before starting an action. Warm that
    # parent-only language reader before varying TEMP beyond the ANSI code page;
    # the fresh child must use its selected language without compiling anything.
    . (Join-Path $repo 'tools/package/gesher-ui.ps1')
    $env:TEMP = $here; $env:TMP = $here
    Copy-Item -LiteralPath (Join-Path $repo 'tools/package/ui-messages.json') -Destination $here
    $ui = [IO.File]::ReadAllText((Join-Path $repo 'tools/package/gesher-ui.ps1'))
    # Declared HTTP boundary: real staging-file writes and command parameter
    # binding are retained, without downloading or installing anything.
    $ui += @'

function Invoke-WebRequest {
    [CmdletBinding()]
    param([string] $Uri, [string] $OutFile, [switch] $UseBasicParsing, [int] $TimeoutSec)
    $cfg = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'fixture.json') -Raw | ConvertFrom-Json
    $call = [PSCustomObject]@{ Parts = @('download', $Uri); Output = $OutFile; Timeout = $TimeoutSec }
    Add-Content -LiteralPath (Join-Path $PSScriptRoot 'calls.log') -Value ($call | ConvertTo-Json -Compress) -Encoding UTF8
    [IO.File]::WriteAllText($OutFile, 'partial')
    if ($cfg.DownloadFails) { throw 'fixture download failed' }
    [IO.File]::WriteAllText($OutFile, 'complete fixture download')
}
'@
    [IO.File]::WriteAllText((Join-Path $here 'gesher-ui.ps1'), $ui, [Text.UTF8Encoding]::new($true))
    . (Join-Path $here 'gesher-ui.ps1')
    $script:GesherUiLanguage = 'en'
    $app = @'
param([Parameter(ValueFromRemainingArguments=$true)][string[]] $Parts)
. (Join-Path $PSScriptRoot 'gesher-ui.ps1')
$cfg = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'fixture.json') -Raw | ConvertFrom-Json
$call = [PSCustomObject]@{ Parts = @($Parts); Process = $PID; Thread = [Threading.Thread]::CurrentThread.ManagedThreadId; Carrier = $env:GESHER_LAUNCH_ERROR_FILE; UiLanguage = $script:GesherUiLanguage; AndroidLocales = $env:GESHER_LOCALES; PanelLanguage = $env:GESHER_PANEL_UI_LANGUAGE }
Add-Content -LiteralPath (Join-Path $PSScriptRoot 'calls.log') -Value ($call | ConvertTo-Json -Compress) -Encoding UTF8
Write-Output 'discarded helper output'
Write-Host 'discarded helper information'
if ($cfg.NativeLogs) { & $env:ComSpec /d /c 'echo successful native diagnostic 1>&2' }
if ($cfg.ExitHost) { [Environment]::Exit(29) }
if ($cfg.LargeOutput) {
    $bytes = [byte[]]::new(8192)
    $stdout = [Console]::OpenStandardOutput(); $stderr = [Console]::OpenStandardError()
    try { for ($i = 0; $i -lt 4096; $i++) { $stdout.Write($bytes,0,$bytes.Length); $stderr.Write($bytes,0,$bytes.Length) } }
    finally { $stdout.Dispose(); $stderr.Dispose() }
}
if ($cfg.Delay) { Start-Sleep -Milliseconds $cfg.Delay }
if ($Parts[0] -eq 'install' -and [IO.Path]::GetFileName($Parts[1]).StartsWith('fail')) {
    [IO.File]::WriteAllText($env:GESHER_LAUNCH_ERROR_FILE, 'different signing key', [Text.UTF8Encoding]::new($false))
    exit 23
}
if ($Parts[0] -eq 'install' -and [IO.Path]::GetFileName($Parts[1]).StartsWith('throw')) { throw 'fixture helper exception' }
exit $cfg.Exit
'@
    [IO.File]::WriteAllText((Join-Path $here 'gesher-app.ps1'), $app, [Text.UTF8Encoding]::new($true))
    $microg = @'
param([switch] $AutoDownload, [switch] $Disable)
$cfg = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'fixture.json') -Raw | ConvertFrom-Json
Add-Content -LiteralPath (Join-Path $PSScriptRoot 'calls.log') -Value ([PSCustomObject]@{ Parts = @('microg', [string]$AutoDownload, [string]$Disable) } | ConvertTo-Json -Compress) -Encoding UTF8
Invoke-WebRequest -Uri 'fixture://microg' -OutFile (Join-Path $PSScriptRoot 'microg.download') -UseBasicParsing
exit $cfg.MicrogExit
'@
    [IO.File]::WriteAllText((Join-Path $here 'gesher-app-microg.ps1'), $microg, [Text.UTF8Encoding]::new($true))

    Reset-Action
    $script:Fixture.Delay = 450; Save-ActionFixture
    $watch = [Diagnostics.Stopwatch]::StartNew()
    Assert-Action (Start-PanelAction 'Device' @{ Command = 'start' }) 'First action was rejected'
    $watch.Stop()
    $worker = $script:ActionState.Worker
    Assert-Action ($worker -and -not $worker.Handle.IsCompleted) 'Start waited for the blocking helper'
    Assert-Action ($script:IsBusy -and -not $btnToggleDevice.IsEnabled -and -not $servicesCards.IsEnabled -and
        -not $cameraCard.IsEnabled -and -not $applicationsSection.IsEnabled -and -not $window.AllowDrop -and -not $btnInstallUpdate.IsEnabled) 'Mutation controls remained enabled'
    Assert-Action (-not (Start-PanelAction 'Device' @{ Command = 'shutdown' })) 'Concurrent mutation was admitted'
    Assert-Action (-not (Test-PanelActionClose)) 'Close abandoned an active mutation'
    Assert-Action ($statusMessage.Text -eq (Get-GesherText 'cp_action_finish')) 'Deferred close did not explain itself'
    Drain-Action
    Assert-Action ($script:PumpTicks -ge 3) 'WPF dispatcher did not keep ticking during helper execution'
    Assert-Action ($worker.Disposed -and -not $script:ActionState.Worker -and -not $script:IsBusy) 'Completed worker was retained or busy state stuck'
    Assert-Action ($btnToggleDevice.IsEnabled -and $servicesCards.IsEnabled -and $cameraCard.IsEnabled -and $applicationsSection.IsEnabled -and $window.AllowDrop) 'Controls did not recover after completion'
    Assert-Action ($script:Refreshes -eq 1 -and $script:Messages.Count -eq 0) "Successful action did not complete once (refreshes=$script:Refreshes errors=$($script:Messages | ConvertTo-Json -Compress))"
    Complete-PanelAction
    Assert-Action ($script:Refreshes -eq 1) 'Completion executed twice'
    $calls = Read-ActionCalls
    Assert-Action ($calls.Count -eq 1 -and $calls[0].Parts[0] -eq 'start' -and $calls[0].Process -ne $PID) 'Device command did not run in its isolated host, or overlapping action ran'
    Write-Host "Panel async measurement: start_ms=$($watch.ElapsedMilliseconds) dispatcher_ticks=$script:PumpTicks helper_delay_ms=450"

    $wide = [IntPtr]::Size -eq 8
    $layouts = @(
        @{ Type = [Gesher.PanelActionSecurity]; Size = $(if ($wide) { 24 } else { 12 }); Offsets = @{ Descriptor = $(if ($wide) { 8 } else { 4 }); Inherit = $(if ($wide) { 16 } else { 8 }) } },
        @{ Type = [Gesher.PanelActionStartup]; Size = $(if ($wide) { 104 } else { 68 }); Offsets = @{ ReservedBytes = $(if ($wide) { 72 } else { 52 }); Input = $(if ($wide) { 80 } else { 56 }); Error = $(if ($wide) { 96 } else { 64 }) } },
        @{ Type = [Gesher.PanelActionExtendedStartup]; Size = $(if ($wide) { 112 } else { 72 }); Offsets = @{ Attributes = $(if ($wide) { 104 } else { 68 }) } },
        @{ Type = [Gesher.PanelActionProcessInfo]; Size = $(if ($wide) { 24 } else { 16 }); Offsets = @{ Thread = [IntPtr]::Size; ProcessId = $(if ($wide) { 16 } else { 8 }); ThreadId = $(if ($wide) { 20 } else { 12 }) } }
    )
    foreach ($layout in $layouts) {
        $valid = [Runtime.InteropServices.Marshal]::SizeOf([type]$layout.Type) -eq $layout.Size
        foreach ($field in $layout.Offsets.Keys) {
            $valid = $valid -and [Runtime.InteropServices.Marshal]::OffsetOf([type]$layout.Type, $field).ToInt32() -eq $layout.Offsets[$field]
        }
        Assert-Action $valid "Native structure layout does not match Win32: $($layout.Type.FullName)"
    }
    $valid = $true
    foreach ($name in 'CreateFileW','InitializeProcThreadAttributeList','UpdateProcThreadAttribute','DeleteProcThreadAttributeList','CreateProcessW','WaitForSingleObject','GetExitCodeProcess','CloseHandle') {
        $method = [Gesher.PanelActionNative].GetMethod($name)
        $native = [Gesher.PanelActionNative].GetMethod(('Native' + $name), [Reflection.BindingFlags]'NonPublic,Static')
        $attribute = $native.GetCustomAttributes([Runtime.InteropServices.DllImportAttribute], $false)[0]
        $valid = $valid -and $attribute.SetLastError -and $attribute.CharSet -eq [Runtime.InteropServices.CharSet]::Unicode -and
            $attribute.CallingConvention -eq [Runtime.InteropServices.CallingConvention]::Winapi -and $attribute.EntryPoint -ceq $name -and
            $method.ReturnType -eq $native.ReturnType -and $method.GetCustomAttributes([Runtime.InteropServices.DllImportAttribute], $false).Count -eq 0
    }
    Assert-Action $valid 'Emitted native imports lost Unicode, Winapi calling convention or last-error preservation'
    $errorField = [Gesher.PanelActionNative].GetField('LastError')
    Assert-Action ($errorField.IsStatic -and $errorField.FieldType -eq [int] -and $errorField.GetCustomAttributes([ThreadStaticAttribute], $false).Count -eq 1) 'Saved native error is not isolated per native thread'
    $probeRunspace = [Management.Automation.Runspaces.RunspaceFactory]::CreateRunspace()
    $probe = [PowerShell]::Create()
    try {
        $probeRunspace.ThreadOptions = [Management.Automation.Runspaces.PSThreadOptions]::ReuseThread
        $probeRunspace.Open(); $probe.Runspace = $probeRunspace
        [Gesher.PanelActionNative]::LastError = 12345
        $null = $probe.AddScript(@'
$initial = [Gesher.PanelActionNative]::LastError
$code = [uint32]0
$ok = [Gesher.PanelActionNative]::GetExitCodeProcess([IntPtr]::Zero, [ref]$code)
[PSCustomObject]@{ Initial = $initial; Ok = $ok; Error = [Gesher.PanelActionNative]::LastError; Thread = [Threading.Thread]::CurrentThread.ManagedThreadId }
'@)
        $result = @($probe.Invoke())
        Assert-Action ($result.Count -eq 1 -and $result[0].Initial -eq 0 -and -not $result[0].Ok -and $result[0].Error -eq 6 -and
            $result[0].Thread -ne $script:UiThread -and [Gesher.PanelActionNative]::LastError -eq 12345) 'Native error wrapper lost ERROR_INVALID_HANDLE or overwrote another thread error'
    } finally { $probe.Dispose(); $probeRunspace.Dispose() }

    foreach ($language in 'he', 'en') {
        Reset-Action
        $env:GESHER_LOCALES = 'fr-FR,en-US'
        $script:GesherUiLanguage = $language
        [void](Start-PanelAction 'Device' @{ Command = 'start' }); Drain-Action
        $calls = Read-ActionCalls
        Assert-Action ($script:Messages.Count -eq 0 -and $calls[0].UiLanguage -eq $language -and $calls[0].PanelLanguage -eq $language) 'Child did not reuse the chosen display language without compiling'
        Assert-Action ($calls[0].AndroidLocales -ceq 'fr-FR,en-US' -and $env:GESHER_LOCALES -ceq 'fr-FR,en-US') 'Display-language reuse changed Android application locales'
        Assert-Action ([Environment]::GetEnvironmentVariable('GESHER_PANEL_UI_LANGUAGE','Process') -ceq $savedEnvironment['GESHER_PANEL_UI_LANGUAGE']) 'Child display-language override leaked into parent environment'
    }
    $script:GesherUiLanguage = 'en'

    Reset-Action
    $literal = Join-Path $here "שלום [x] '{1};`$data.apk"
    $outer = Join-Path $here 'outer-error.txt'
    [IO.File]::WriteAllText($outer, 'keep caller reason')
    $env:GESHER_LAUNCH_ERROR_FILE = $outer
    Assert-Action (Start-PanelAction 'Install' @{ Files = @((Join-Path $here 'fail1.apk'), $literal, (Join-Path $here 'throw2.apk'), (Join-Path $here 'fail3.apk')) }) 'Batch install did not start'
    Drain-Action
    $calls = Read-ActionCalls
    Assert-Action ($calls.Count -eq 4 -and $calls[1].Parts[1] -ceq $literal) 'Batch stopped early or interpreted a literal path'
    Assert-Action ($script:Messages.Count -eq 1 -and $script:Messages[0].Message.Contains('fail1.apk') -and
        $script:Messages[0].Message.Contains('fail3.apk') -and $script:Messages[0].Message.Contains('different signing key') -and
        $script:Messages[0].Message.Contains('fixture helper exception')) 'Batch error reasons were lost'
    Assert-Action ($script:Messages[0].Thread -eq $script:UiThread) 'Worker displayed an error off the dispatcher'
    Assert-Action ($env:GESHER_LAUNCH_ERROR_FILE -ceq $outer -and [IO.File]::ReadAllText($outer) -ceq 'keep caller reason') 'Worker changed the parent install diagnostic carrier'
    foreach ($call in $calls) { Assert-Action (-not (Test-Path -LiteralPath $call.Carrier)) 'Temporary install diagnostic was retained' }

    Reset-Action
    $script:Fixture.LargeOutput = $true; Save-ActionFixture
    [void](Start-PanelAction 'Device' @{ Command = 'start' }); Drain-Action
    Assert-Action ($script:Messages.Count -eq 0) 'Native stderr or large discarded output turned a successful helper into failure'
    Assert-Action (@(Get-ChildItem -LiteralPath $here -Directory -Filter 'gesher-panel-action-*').Count -eq 0) 'Owned host request/result staging was retained'

    Reset-Action
    $script:Fixture.ExitHost = $true; Save-ActionFixture
    [void](Start-PanelAction 'Device' @{ Command = 'start' }); Drain-Action
    Assert-Action ($script:Messages.Count -eq 1 -and $script:Messages[0].Message.Contains('29')) 'An exited helper host was acknowledged without a result'
    Assert-Action (-not $script:IsBusy -and -not $script:ActionState.Worker) 'Exited host retained the action guard'
    Assert-Action (@(Get-ChildItem -LiteralPath $here -Directory -Filter 'gesher-panel-action-*').Count -eq 0) 'Exited host retained its owned staging files'

    Reset-Action
    $savedHere = $here
    try {
        # CreateProcess must clean the explicitly inherited NUL handles and
        # staging files when the native process cannot even be started.
        $here = Join-Path $here 'missing-working-directory'
        Assert-Action (Start-PanelAction 'Device' @{ Command = 'start' }) 'Failed-start fixture was not admitted'
        $worker = $script:ActionState.Worker
    } finally { $here = $savedHere }
    Drain-Action
    Assert-Action ($script:Messages.Count -eq 1 -and $worker.Disposed -and -not $script:IsBusy -and
        -not $script:ActionState.Worker -and (Read-ActionCalls).Count -eq 0) 'Native process-start failure retained ownership or ran a helper'
    Assert-Action (@(Get-ChildItem -LiteralPath $here -Directory -Filter 'gesher-panel-action-*').Count -eq 0) 'Failed process start retained its owned staging files'

    $process = [Diagnostics.Process]::GetCurrentProcess()
    try {
        # Isolate native cleanup from reflection, PowerShell binding and process
        # census initialization. Require real Win32 errors rather than swallowing
        # missing-method/type errors that could make this check ineffective.
        function Invoke-FailedNativeStart {
            try { [void](Invoke-PanelActionNative (Join-Path $here 'missing.exe') 'unused.ps1' $here) }
            catch {
                $failure = $_.Exception.GetBaseException()
                if ($failure -is [ComponentModel.Win32Exception] -and $failure.NativeErrorCode -eq 2) { $script:NativeFailureCount++; return }
                throw
            }
            throw 'Missing native executable unexpectedly started'
        }
        $script:NativeFailureCount = 0
        for ($attempt = 0; $attempt -lt 32; $attempt++) { Invoke-FailedNativeStart }
        $null = $process.Handle
        [GC]::Collect(); [GC]::WaitForPendingFinalizers(); [GC]::Collect()
        $process.Refresh(); $handlesBefore = $process.HandleCount
        $script:NativeFailureCount = 0
        for ($attempt = 0; $attempt -lt 32; $attempt++) { Invoke-FailedNativeStart }
        [GC]::Collect(); [GC]::WaitForPendingFinalizers(); [GC]::Collect()
        $process.Refresh()
        Write-Host "Panel failed-start handles: before=$handlesBefore after=$($process.HandleCount) attempts=32"
        Assert-Action ($script:NativeFailureCount -eq 32) 'Native census did not exercise 32 real ERROR_FILE_NOT_FOUND failures'
        Assert-Action ($process.HandleCount -eq $handlesBefore) 'Native failed-start attempts changed the warmed native handle balance'

        # Demonstrate that the exact census rejects an owned handle leak, then
        # restore ownership before continuing the remaining action contracts.
        $security = [Gesher.PanelActionSecurity]::new()
        $security.Length = [Runtime.InteropServices.Marshal]::SizeOf([type][Gesher.PanelActionSecurity])
        function Open-NulForCensus { [Gesher.PanelActionNative]::CreateFileW('NUL',1073741824,3,[ref]$security,3,0,[IntPtr]::Zero) }
        $warmHandle = Open-NulForCensus
        [void][Gesher.PanelActionNative]::CloseHandle($warmHandle)
        [GC]::Collect(); [GC]::WaitForPendingFinalizers(); [GC]::Collect()
        $process.Refresh(); $negativeBaseline = $process.HandleCount
        $leaked = Open-NulForCensus
        try {
            Assert-Action ($leaked -ne [IntPtr]::Zero -and $leaked -ne [IntPtr]::new(-1)) 'Could not create the handle-census negative control'
            $process.Refresh()
            Write-Host "Panel handle negative control: before=$negativeBaseline retained=$($process.HandleCount)"
            Assert-Action ($process.HandleCount -eq $negativeBaseline + 1) 'Handle census failed to detect one deliberately retained NUL handle'
        } finally { $closed = [Gesher.PanelActionNative]::CloseHandle($leaked) }
        Assert-Action $closed 'Could not release the handle-census negative control'
        [GC]::Collect(); [GC]::WaitForPendingFinalizers(); [GC]::Collect()
        $process.Refresh()
        Write-Host "Panel handle negative cleanup: after=$($process.HandleCount)"
        Assert-Action ($process.HandleCount -eq $negativeBaseline) 'Handle negative-control cleanup did not restore ownership balance'
    } finally { $process.Dispose() }

    foreach ($enable in $true, $false) {
        Reset-Action
        Assert-Action (Start-PanelAction 'Microg' @{ Enable = $enable }) 'microG action did not start'
        Drain-Action
        $calls = Read-ActionCalls
        Assert-Action (($calls | ForEach-Object { $_.Parts[0] }) -join ',' -eq 'shutdown,microg,download,start') 'microG mutation order changed'
        Assert-Action ($calls[1].Parts[1] -eq [string]$enable -and $calls[1].Parts[2] -eq [string](-not $enable)) 'microG switches changed'
        Assert-Action ($calls[2].Timeout -eq 120) 'Nested helper transport wait remained unbounded'
    }
    Reset-Action
    $script:Fixture.MicrogExit = 7; Save-ActionFixture
    [void](Start-PanelAction 'Microg' @{ Enable = $true }); Drain-Action
    Assert-Action ((Read-ActionCalls).Count -eq 3 -and $script:Messages[0].Message.Contains('7')) 'Failed microG staging silently restarted the device'

    Reset-Action
    $script:Fixture.Exit = 9; Save-ActionFixture
    [void](Start-PanelAction 'Uninstall' @{ Package = 'fixture.package'; Label = 'Literal {1} label' }); Drain-Action
    Assert-Action ($script:Messages[0].Message -ceq (Get-GesherText 'cp_uninstall_failed' @('Literal {1} label'))) 'Uninstall failure lost its literal label'

    $store = Join-Path $here 'AuroraStore.apk'
    foreach ($fail in $true, $false) {
        Reset-Action
        $script:Fixture.DownloadFails = $fail; Save-ActionFixture
        [void](Start-PanelAction 'Store' @{}); Drain-Action
        $calls = Read-ActionCalls
        Assert-Action ($calls[0].Parts[0] -eq 'download' -and $calls[0].Timeout -eq 120) 'Store download was not bounded'
        Assert-Action (-not (Test-Path -LiteralPath $calls[0].Output)) 'Partial download was retained'
        if ($fail) {
            Assert-Action ($calls.Count -eq 1 -and -not (Test-Path -LiteralPath $store) -and $script:Messages.Count -eq 1) 'Failed download was published or installed'
        } else {
            Assert-Action ($calls.Count -eq 2 -and [IO.File]::ReadAllText($store) -eq 'complete fixture download') 'Completed store download was not published then installed'
        }
    }
    Reset-Action
    [void](Start-PanelAction 'Store' @{}); Drain-Action
    Assert-Action ((Read-ActionCalls).Count -eq 1) 'Existing store cache was needlessly downloaded again'

    Reset-Action
    $script:FailRefresh = $true
    [void](Start-PanelAction 'Device' @{ Command = 'start' })
    $failed = $false
    try { Drain-Action } catch { $failed = $_.Exception.Message.Contains('fixture refresh failed') }
    Assert-Action ($failed -and -not $script:IsBusy -and -not $script:ActionState.Completing -and -not $script:ActionState.Worker) 'Refresh failure retained the mutation guard or worker'

    Reset-Action
    $script:UpdateState.Phase = 'Confirming'
    Assert-Action (-not (Start-PanelAction 'Device' @{ Command = 'start' })) 'Dashboard mutation overlapped setup confirmation'
    $script:UpdateState.Phase = 'Installing'
    Assert-Action (-not (Start-PanelAction 'Device' @{ Command = 'start' })) 'Dashboard mutation overlapped setup installation'
    $script:UpdateState.Phase = 'Idle'; $script:ActionState.Closed = $true
    Assert-Action (-not (Start-PanelAction 'Device' @{ Command = 'start' })) 'Closed panel started a worker'
    Assert-Action (Test-PanelActionClose) 'Idle panel could not close'
    "PASS: control panel asynchronous action contracts ($script:Checks checks)"
} finally {
    if ($script:ActionState.Worker) {
        # A failed fixture may still own a short, controlled helper. Drain it
        # before disposing: test cleanup never aborts an installation mid-call.
        $script:FailRefresh = $false
        Drain-Action
    }
    $window.Close()
    foreach ($name in $savedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process') }
    $full = [IO.Path]::GetFullPath($here)
    if (-not $full.StartsWith($scratchRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected fixture cleanup path' }
    if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
}
