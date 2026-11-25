param([Parameter(Mandatory = $true)][string] $LauncherPath)

$ErrorActionPreference = 'Stop'
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw 'This launcher regression requires Windows.' }
$LauncherPath = (Get-Item -LiteralPath $LauncherPath -ErrorAction Stop).FullName
$script:LauncherChecks = 0
function Assert-PanelLauncher([bool] $Condition, [string] $Message) {
    if (-not $Condition) { throw $Message }
    $script:LauncherChecks++
}

# Explicit NULL standard handles reproduce Explorer, independently of the CI
# runner's own redirected output. Retain a real process handle even if the
# native parent exits before Process.GetProcessById could find it.
if (-not ('Gesher.PanelLauncherProbe.NativeProcess' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
namespace Gesher.PanelLauncherProbe {
    public sealed class NativeProcess : IDisposable {
        [StructLayout(LayoutKind.Sequential)] struct StartupInfo {
            public int cb;
            public IntPtr reserved, desktop, title;
            public int x, y, width, height, charsX, charsY, fill, flags;
            public short show, reservedSize;
            public IntPtr reservedBytes, input, output, error;
        }
        [StructLayout(LayoutKind.Sequential)] struct ProcessInfo {
            public IntPtr process, thread;
            public uint processId, threadId;
        }
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool CreateProcessW(string application, StringBuilder command,
            IntPtr processAttributes, IntPtr threadAttributes, bool inherit,
            uint flags, IntPtr environment, string directory, ref StartupInfo startup, out ProcessInfo process);
        [DllImport("kernel32.dll", SetLastError = true)] static extern uint WaitForSingleObject(IntPtr handle, uint timeout);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetExitCodeProcess(IntPtr handle, out uint code);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool TerminateProcess(IntPtr handle, uint code);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
        IntPtr handle;
        public uint Id { get; private set; }
        public static NativeProcess Start(string executable, string arguments, string directory) {
            var startup = new StartupInfo();
            startup.cb = Marshal.SizeOf(typeof(StartupInfo));
            startup.flags = 0x100; // STARTF_USESTDHANDLES: all three handles stay NULL.
            ProcessInfo process;
            if (!CreateProcessW(executable, new StringBuilder("\"" + executable + "\" " + arguments),
                IntPtr.Zero, IntPtr.Zero, false, 0x08000000, IntPtr.Zero, directory, ref startup, out process))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            CloseHandle(process.thread);
            return new NativeProcess { handle = process.process, Id = process.processId };
        }
        public bool Wait(int milliseconds) {
            uint result = WaitForSingleObject(handle, (uint)milliseconds);
            if (result == 0) return true;
            if (result == 258) return false;
            throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        public int ExitCode {
            get {
                uint result;
                if (!GetExitCodeProcess(handle, out result)) throw new Win32Exception(Marshal.GetLastWin32Error());
                return unchecked((int)result);
            }
        }
        public void Kill() { if (!Wait(0)) TerminateProcess(handle, 99); }
        public void Dispose() { if (handle != IntPtr.Zero) { CloseHandle(handle); handle = IntPtr.Zero; } }
    }
}
'@
}

$standIn = @'
$ErrorActionPreference = 'Stop'
$identity = [pscustomobject]@{ ProcessId = $PID; StartTime = [Diagnostics.Process]::GetCurrentProcess().StartTime.ToFileTimeUtc() }
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'child.json'), ($identity | ConvertTo-Json -Compress))
try {
    Add-Type -Namespace PanelChildProbe -Name Native -MemberDefinition '[DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();'
    $ready = [pscustomobject]@{
        ProcessId = $identity.ProcessId; StartTime = $identity.StartTime
        ConsoleWindow = [PanelChildProbe.Native]::GetConsoleWindow().ToInt64()
        ScriptPath = $PSCommandPath
        Arguments = @($args)
    }
    $temporary = Join-Path $PSScriptRoot 'ready.tmp'
    [IO.File]::WriteAllText($temporary, ($ready | ConvertTo-Json -Compress))
    [IO.File]::Move($temporary, (Join-Path $PSScriptRoot 'ready.json'))
    $deadline = [DateTime]::UtcNow.AddSeconds(90)
    while (-not [IO.File]::Exists((Join-Path $PSScriptRoot 'release'))) {
        if ([DateTime]::UtcNow -gt $deadline) { throw 'The test never released its child process.' }
        Start-Sleep -Milliseconds 50
    }
    'control-panel launcher probe: output passed on'
    exit 23
} catch {
    [IO.File]::WriteAllText((Join-Path $PSScriptRoot 'failure.txt'), ($_ | Out-String))
    exit 99
}
'@

function Wait-PanelProbe([string] $Directory) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt 45000) {
        $failure = Join-Path $Directory 'failure.txt'
        if ([IO.File]::Exists($failure)) { throw [IO.File]::ReadAllText($failure) }
        $ready = Join-Path $Directory 'ready.json'
        if ([IO.File]::Exists($ready)) { return [IO.File]::ReadAllText($ready) | ConvertFrom-Json }
        Start-Sleep -Milliseconds 50
    }
    throw 'The stand-in control panel did not start within 45 seconds.'
}

function Get-PanelProbeProcess([object] $Record) {
    $process = Get-Process -Id $Record.ProcessId -ErrorAction Stop
    try {
        # Retain the kernel process object even after the detached native parent
        # exits; a temporary WaitForExit handle alone cannot preserve ExitCode.
        $null = $process.Handle
        if ($process.StartTime.ToFileTimeUtc() -ne $Record.StartTime) { throw 'The probe process identity changed.' }
        return $process
    } catch {
        $process.Dispose()
        throw
    }
}

function Assert-LauncherReplaceable([string] $Path) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        try {
            $file = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
            $file.Dispose()
            Copy-Item -LiteralPath $LauncherPath -Destination $Path -Force -ErrorAction Stop
            return
        } catch {
            if ($watch.ElapsedMilliseconds -ge 5000) { throw }
            Start-Sleep -Milliseconds 50
        }
    }
}

function Invoke-PanelLauncherCase([string] $Command, [string] $Directory, [bool] $Detached,
        [switch] $Redirect, [switch] $DirectPanel, [switch] $AppLayout, [switch] $NoArguments) {
    $null = [IO.Directory]::CreateDirectory($Directory)
    $executable = Join-Path $Directory 'gesher-launch.exe'
    Copy-Item -LiteralPath $LauncherPath -Destination $executable
    $scriptDirectory = if ($AppLayout) { Join-Path $Directory 'App' } else { $Directory }
    [void][IO.Directory]::CreateDirectory($scriptDirectory)
    [IO.File]::WriteAllText((Join-Path $scriptDirectory 'gesher-app.ps1'), $standIn)
    if ($DirectPanel) {
        [IO.File]::WriteAllText((Join-Path $scriptDirectory 'gesher-control-panel.ps1'), $standIn)
    }
    $isPanel = $NoArguments -or $Command -in 'control-panel', 'settings', 'dashboard', 'gui'
    $usesDirectPanel = $DirectPanel -and $isPanel
    $expectedScript = Join-Path $scriptDirectory $(if ($usesDirectPanel) { 'gesher-control-panel.ps1' } else { 'gesher-app.ps1' })
    $expectedArguments = if ($usesDirectPanel) { '' } elseif ($NoArguments) { 'control-panel' } else { $Command + '|argument with spaces' }
    $parent = $null
    $child = $null
    try {
        $arguments = if ($NoArguments) { '' } else { $Command + ' "argument with spaces"' }
        if ($Redirect) {
            $info = New-Object Diagnostics.ProcessStartInfo
            $info.FileName = $executable
            $info.Arguments = $arguments
            $info.WorkingDirectory = $Directory
            $info.UseShellExecute = $false
            $info.CreateNoWindow = $true
            $info.RedirectStandardOutput = $true
            $info.RedirectStandardError = $true
            $parent = [Diagnostics.Process]::Start($info)
            $stdout = $parent.StandardOutput.ReadToEndAsync()
            $stderr = $parent.StandardError.ReadToEndAsync()
        } else {
            $parent = [Gesher.PanelLauncherProbe.NativeProcess]::Start($executable, $arguments, $Directory)
        }
        $record = Wait-PanelProbe $scriptDirectory
        $child = Get-PanelProbeProcess $record
        Assert-PanelLauncher ($record.ConsoleWindow -eq 0) "$Command flashed a PowerShell console window."
        Assert-PanelLauncher ($record.ScriptPath -ceq $expectedScript) "$Command selected the wrong entry script."
        Assert-PanelLauncher (($record.Arguments -join '|') -ceq $expectedArguments) "$Command changed its arguments."
        Assert-PanelLauncher (-not $child.HasExited) "$Command did not keep the control panel running."
        if ($Detached) {
            Assert-PanelLauncher ($parent.Wait(5000)) "$Command kept the native launcher alive while the panel was open."
            Assert-PanelLauncher ($parent.ExitCode -eq 0) "$Command did not report a successful handoff."
            Assert-LauncherReplaceable $executable
            Assert-PanelLauncher (-not $child.HasExited) "$Command closed the panel while replacing its launcher."
        } else {
            $ended = if ($Redirect) { $parent.WaitForExit(250) } else { $parent.Wait(250) }
            Assert-PanelLauncher (-not $ended) "$Command stopped waiting for the script's exit code."
        }
        [IO.File]::WriteAllText((Join-Path $scriptDirectory 'release'), 'go')
        Assert-PanelLauncher ($child.WaitForExit(15000)) "$Command left the child running after its gate opened."
        Assert-PanelLauncher ($child.ExitCode -eq 23) "$Command child did not complete with its deliberate exit code."
        if (-not $Detached) {
            $ended = if ($Redirect) { $parent.WaitForExit(15000) } else { $parent.Wait(15000) }
            Assert-PanelLauncher $ended "$Command native launcher did not exit after the script."
            Assert-PanelLauncher ($parent.ExitCode -eq 23) "$Command did not forward the script's exit code."
        }
        if ($Redirect) {
            Assert-PanelLauncher ($stdout.Wait(15000) -and $stderr.Wait(15000)) 'Redirected launcher output did not close.'
            Assert-PanelLauncher (($stdout.Result + $stderr.Result).Contains('control-panel launcher probe: output passed on')) 'Redirected control-panel output was lost.'
        }
    } finally {
        # Also find a child that failed before publishing ready.json, so a failed
        # assertion cannot leave a gated PowerShell process on the runner.
        [IO.File]::WriteAllText((Join-Path $scriptDirectory 'release'), 'cleanup')
        if (-not $child -and [IO.File]::Exists((Join-Path $scriptDirectory 'child.json'))) {
            try { $child = Get-PanelProbeProcess ([IO.File]::ReadAllText((Join-Path $scriptDirectory 'child.json')) | ConvertFrom-Json) } catch { }
        }
        if ($child) {
            if (-not $child.WaitForExit(10000)) { $child.Kill(); $null = $child.WaitForExit(10000) }
            $child.Dispose()
        }
        if ($parent) {
            if ($Redirect) {
                if (-not $parent.WaitForExit(1000)) { $parent.Kill(); $null = $parent.WaitForExit(10000) }
            } else { $parent.Kill() }
            $parent.Dispose()
        }
    }
}

$work = Join-Path ([IO.Path]::GetTempPath()) ('gesher-panel-launch-' + [guid]::NewGuid().ToString('N'))
try {
    # Windows paths ignore case: settings and SeTtInGs must not share the
    # previous child's ready.json, identity record or release gate.
    $caseIndex = 0
    foreach ($command in 'control-panel', 'settings', 'dashboard', 'gui', 'SeTtInGs') {
        $caseIndex++
        Invoke-PanelLauncherCase -Command $command -Directory (Join-Path $work ('detached {0} {1}' -f $caseIndex, $command)) -Detached $true
        Invoke-PanelLauncherCase -Command $command -Directory (Join-Path $work ('direct {0} {1}' -f $caseIndex, $command)) -Detached $true -DirectPanel
    }
    Invoke-PanelLauncherCase -Command 'control-panel' -Directory (Join-Path $work 'redirected') -Detached $false -Redirect
    Invoke-PanelLauncherCase -Command 'control-panel' -Directory (Join-Path $work 'direct redirected') -Detached $false -Redirect -DirectPanel
    Invoke-PanelLauncherCase -Directory (Join-Path $work 'default fallback') -Detached $true -NoArguments
    Invoke-PanelLauncherCase -Directory (Join-Path $work 'default direct') -Detached $true -NoArguments -DirectPanel
    Invoke-PanelLauncherCase -Directory (Join-Path $work 'default App layout') -Detached $true -NoArguments -DirectPanel -AppLayout
    Invoke-PanelLauncherCase -Command 'settings' -Directory (Join-Path $work 'alias App layout') -Detached $true -DirectPanel -AppLayout
    # A command that does not open the dashboard retains its old wait behavior,
    # even without handles. App/splash behavior has its separate existing probes.
    Invoke-PanelLauncherCase -Command 'status' -Directory (Join-Path $work 'ordinary') -Detached $false -DirectPanel
    'PASS  shell: control-panel handoff frees the native launcher, keeps its child alive and preserves redirected waits ({0} checks)' -f $script:LauncherChecks
} finally {
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}
