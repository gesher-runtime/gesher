# Run the actual packaged Inno Setup executable. These checks stop at the Ready
# page; the package job separately installs with explicit consent and then
# upgrades without a new consent flag. No runtime or consent function is mocked.
[CmdletBinding()]
param(
    [string] $InstallerPath = '',
    # Compile the unchanged installer with inert payload bytes for a fast host
    # gate. These checks never click Install or execute any runtime payload.
    [switch] $BuildFixture,
    [string] $LicensePath = (Join-Path $PSScriptRoot '../../../LICENSE'),
    [string] $InnoDirectory = "${env:ProgramFiles(x86)}\Inno Setup 6"
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'InstallerLicenseChecks requires Windows and the compiled production installer.'
}
$LicensePath = (Resolve-Path -LiteralPath $LicensePath).ProviderPath
$licenseBytes = [IO.File]::ReadAllBytes($LicensePath)
$licenseText = [IO.File]::ReadAllText($LicensePath).Replace("`r`n", "`n").TrimEnd("`n")
$licenseHash = (Get-FileHash -LiteralPath $LicensePath -Algorithm SHA256).Hash.ToLowerInvariant()
$work = Join-Path ([IO.Path]::GetTempPath()) ('gesher-license-checks-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $work
$script:checks = 0
$script:sequence = 0
$script:running = $null

# Observe and operate only windows belonging to the installer process tree
# started by this test. SendMessageTimeout bounds reads of another process.
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
namespace Gesher.LicenseTest {
  public sealed class Control {
    public IntPtr Handle;
    public string ClassName, Text;
    public bool Enabled;
    public int Id;
  }
  public static class Windows {
    delegate bool EnumProc(IntPtr window, IntPtr value);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback, IntPtr value);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr value);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr window);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr window, StringBuilder value, int size);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)]
    static extern IntPtr TextMessage(IntPtr window, uint message, IntPtr count, StringBuilder value, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")]
    static extern IntPtr Message(IntPtr window, uint message, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll")] static extern bool PostMessageW(IntPtr window, uint message, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern int GetWindowLongW(IntPtr window, int index);
    public static long Send(IntPtr window, uint message, int w = 0, int l = 0) {
      IntPtr result;
      if (Message(window, message, (IntPtr)w, (IntPtr)l, 2, 2000, out result) == IntPtr.Zero)
        throw new InvalidOperationException("Installer control did not answer: " + message);
      return result.ToInt64();
    }
    public static string Text(IntPtr window) {
      long length = Send(window, 0x000E); // WM_GETTEXTLENGTH
      if (length < 0 || length > 2000000) throw new InvalidOperationException("Invalid control text length");
      var text = new StringBuilder((int)length + 1);
      IntPtr result;
      if (TextMessage(window, 0x000D, (IntPtr)text.Capacity, text, 2, 2000, out result) == IntPtr.Zero)
        throw new InvalidOperationException("Installer text read timed out");
      return text.ToString();
    }
    public static void SetText(IntPtr window, string text) {
      IntPtr result;
      if (TextMessage(window, 0x000C, IntPtr.Zero, new StringBuilder(text), 2, 2000, out result) == IntPtr.Zero)
        throw new InvalidOperationException("Installer directory edit timed out");
    }
    static string ClassName(IntPtr window) {
      var text = new StringBuilder(256);
      GetClassNameW(window, text, text.Capacity);
      return text.ToString();
    }
    public static IntPtr[] TopLevel(int[] owners) {
      var windows = new List<IntPtr>();
      EnumWindows(delegate(IntPtr window, IntPtr unused) {
        uint process;
        GetWindowThreadProcessId(window, out process);
        if (IsWindowVisible(window) && Array.IndexOf(owners, (int)process) >= 0) windows.Add(window);
        return true;
      }, IntPtr.Zero);
      return windows.ToArray();
    }
    public static Control[] Children(IntPtr parent) {
      var controls = new List<Control>();
      EnumChildWindows(parent, delegate(IntPtr window, IntPtr unused) {
        if (IsWindowVisible(window)) controls.Add(new Control { Handle = window,
          ClassName = ClassName(window), Text = Text(window), Enabled = IsWindowEnabled(window), Id = GetDlgCtrlID(window) });
        return true;
      }, IntPtr.Zero);
      return controls.ToArray();
    }
    public static bool IsWizard(IntPtr window) { return ClassName(window) == "TWizardForm"; }
    public static void Click(IntPtr window) {
      if (!IsWindowEnabled(window)) throw new InvalidOperationException("Attempt to click a disabled control");
      if (!PostMessageW(window, 0x00F5, IntPtr.Zero, IntPtr.Zero)) // BM_CLICK
        throw new InvalidOperationException("Installer control click failed");
    }
  }
}
'@

function Assert-License([bool] $Condition, [string] $Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
}
function Wait-License([scriptblock] $Condition, [string] $Message, [int] $Seconds = 10) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $value = & $Condition
        if ($value) { return $value }
        Start-Sleep -Milliseconds 100
    } while ($watch.Elapsed.TotalSeconds -lt $Seconds)
    throw $Message
}
function Caption([string] $Text) { $Text.Replace('&', '') }
function Messages([string] $Language) {
    $values = @{}
    $paths = @((Join-Path $InnoDirectory 'Default.isl'))
    if ($Language -eq 'he') { $paths += Join-Path $InnoDirectory 'Languages/Hebrew.isl' }
    foreach ($path in $paths) {
        foreach ($line in [IO.File]::ReadAllLines($path)) {
            if ($line -match '^([A-Za-z][A-Za-z0-9]*)=(.*)$') { $values[$Matches[1]] = Caption $Matches[2] }
        }
    }
    foreach ($name in 'ButtonNext', 'ButtonBack', 'ButtonCancel', 'ButtonInstall', 'ButtonYes', 'LicenseAccepted', 'LicenseNotAccepted', 'DirExistsTitle', 'DirDoesntExistTitle') {
        if (-not $values.ContainsKey($name)) { throw "Missing Inno Setup message $name" }
    }
    $values
}
function Process-Owners([Diagnostics.Process] $Process) {
    $owners = [Collections.Generic.HashSet[int]]::new()
    $null = $owners.Add($Process.Id)
    $processes = @(Get-CimInstance Win32_Process | Select-Object ProcessId, ParentProcessId)
    do {
        $changed = $false
        foreach ($child in $processes) {
            if ($owners.Contains([int]$child.ParentProcessId) -and $owners.Add([int]$child.ProcessId)) { $changed = $true }
        }
    } while ($changed)
    return ,([int[]]@($owners))
}
function Start-Installer([string] $Directory, [string] $Language, [string[]] $Extra = @()) {
    $script:sequence++
    $arguments = @('/NORESTART', '/SUPPRESSMSGBOXES', "/LANG=$Language", ('/DIR="' + $Directory + '"'),
        ('/LOG="' + (Join-Path $work ($script:sequence.ToString() + '.log')) + '"')) + $Extra
    $script:running = Start-Process -FilePath $InstallerPath -ArgumentList $arguments -PassThru
    return $script:running
}
function Window([Diagnostics.Process] $Process) {
    Wait-License {
        foreach ($window in [Gesher.LicenseTest.Windows]::TopLevel((Process-Owners $Process))) {
            if ([Gesher.LicenseTest.Windows]::IsWizard($window)) { return $window }
        }
        if ($Process.HasExited) { throw "Setup exited before showing its wizard: $($Process.ExitCode)" }
    } 'Setup did not show its wizard' 45
}
function Find-Control([IntPtr] $Window, [string] $Text) {
    $matches = @([Gesher.LicenseTest.Windows]::Children($Window) | Where-Object { (Caption $_.Text) -ceq $Text })
    if ($matches.Count -ne 1) { throw "Expected one installer control '$Text', found $($matches.Count)" }
    $matches[0]
}
function Read-License([IntPtr] $Window) {
    @([Gesher.LicenseTest.Windows]::Children($Window) | Where-Object { $_.Text.StartsWith($licenseText.Split("`n")[0]) })
}
function Confirm-DirectoryDialog([IntPtr] $Window, $Text) {
    if ([Gesher.LicenseTest.Windows]::IsWindowEnabled($Window)) { return }
    foreach ($dialog in [Gesher.LicenseTest.Windows]::TopLevel((Process-Owners $script:running))) {
        if ($dialog -eq $Window) { continue }
        $controls = @([Gesher.LicenseTest.Windows]::Children($dialog))
        $captions = @([Gesher.LicenseTest.Windows]::Text($dialog)) + @($controls | ForEach-Object { $_.Text })
        if (@($captions | Where-Object { $_ -ceq $Text.DirExistsTitle -or $_ -ceq $Text.DirDoesntExistTitle }).Count) {
            foreach ($control in $controls) {
                if ($control.Id -eq 6 -or (Caption $control.Text) -ceq $Text.ButtonYes) {
                    [Gesher.LicenseTest.Windows]::Click($control.Handle)
                    return
                }
            }
        }
    }
}
function Advance-ToAgreementOrReady([IntPtr] $Window, $Text) {
    for ($page = 0; $page -lt 8; $page++) {
        $agreement = @(Read-License $Window)
        if ($agreement.Count) { return 'agreement' }
        $controls = @([Gesher.LicenseTest.Windows]::Children($Window))
        if (@($controls | Where-Object { (Caption $_.Text) -ceq $Text.ButtonInstall }).Count) { return 'ready' }
        $next = Find-Control $Window $Text.ButtonNext
        [Gesher.LicenseTest.Windows]::Click($next.Handle)
        # A page turn changes its visible child handles/text; wait for that turn
        # before deciding whether the next page is the license page.
        $before = ($controls | ForEach-Object { "$($_.Handle):$($_.Text)" }) -join "`n"
        $null = Wait-License {
            Confirm-DirectoryDialog $Window $Text
            $after = ([Gesher.LicenseTest.Windows]::Children($Window) | ForEach-Object { "$($_.Handle):$($_.Text)" }) -join "`n"
            $after -cne $before
        } 'Setup did not advance to its next page'
    }
    throw 'Setup never reached the agreement or Ready page'
}
function Check-Agreement([IntPtr] $Window, $Text) {
    $memo = @(Read-License $Window)
    Assert-License ($memo.Count -eq 1) 'The complete agreement was not visible in one text viewer'
    $actual = $memo[0].Text.Replace("`r`n", "`n").TrimEnd("`n")
    Assert-License ($actual -ceq $licenseText) 'The displayed agreement differs from the complete repository LICENSE'
    Assert-License (([Gesher.LicenseTest.Windows]::GetWindowLongW($memo[0].Handle, -20) -band 0x00402000) -eq 0) 'The English agreement inherits a right-to-left reading/layout style'
    $firstLine = [Gesher.LicenseTest.Windows]::Send($memo[0].Handle, 0x00CE) # EM_GETFIRSTVISIBLELINE
    $null = [Gesher.LicenseTest.Windows]::Send($memo[0].Handle, 0x00B6, 0, 100000) # EM_LINESCROLL
    Assert-License ([Gesher.LicenseTest.Windows]::Send($memo[0].Handle, 0x00CE) -gt $firstLine) 'The full agreement cannot be scrolled'
    $null = [Gesher.LicenseTest.Windows]::Send($memo[0].Handle, 0x00B6, 0, -100000)
    $accept = Find-Control $Window $Text.LicenseAccepted
    $decline = Find-Control $Window $Text.LicenseNotAccepted
    $next = Find-Control $Window $Text.ButtonNext
    Assert-License ([Gesher.LicenseTest.Windows]::Send($accept.Handle, 0x00F0) -eq 0) 'Agreement was accepted by default'
    Assert-License ([Gesher.LicenseTest.Windows]::Send($decline.Handle, 0x00F0) -eq 1) 'Decline was not selected by default'
    Assert-License (-not $next.Enabled) 'Next was enabled without agreement consent'
    [Gesher.LicenseTest.Windows]::Click($accept.Handle)
    $null = Wait-License { [Gesher.LicenseTest.Windows]::IsWindowEnabled($next.Handle) } 'Acceptance did not enable Next'
    [Gesher.LicenseTest.Windows]::Click($decline.Handle)
    $null = Wait-License { -not [Gesher.LicenseTest.Windows]::IsWindowEnabled($next.Handle) } 'Declining did not disable Next'
    $script:checks += 2
}
function Cancel-Installer([Diagnostics.Process] $Process, [IntPtr] $Window, $Text) {
    [Gesher.LicenseTest.Windows]::Click((Find-Control $Window $Text.ButtonCancel).Handle)
    # /SUPPRESSMSGBOXES applies only to silent runs. Confirm the interactive
    # exit prompt using only this test process's own Yes button.
    $null = Wait-License {
        if ($Process.HasExited) { return $true }
        foreach ($dialog in [Gesher.LicenseTest.Windows]::TopLevel((Process-Owners $Process))) {
            if ($dialog -eq $Window) { continue }
            foreach ($control in [Gesher.LicenseTest.Windows]::Children($dialog)) {
                if ($control.Id -eq 6 -or (Caption $control.Text) -ceq $Text.ButtonYes) { [Gesher.LicenseTest.Windows]::Click($control.Handle) }
            }
        }
        return $Process.HasExited
    } 'Setup did not cancel' 15
    Assert-License ($Process.ExitCode -ne 0) 'Cancelling Setup reported a successful installation'
    $Process.Dispose()
    $script:running = $null
}
function Snapshot([string] $Directory) {
    if (-not (Test-Path -LiteralPath $Directory)) { return 'absent' }
    (@(Get-ChildItem -LiteralPath $Directory -Recurse -Force | Sort-Object FullName | ForEach-Object {
        $hash = if ($_.PSIsContainer) { 'directory' } else { (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
        "$($_.FullName):$hash"
    }) -join "`n")
}
function Fixture([string] $Name, [string] $Receipt = '') {
    $directory = Join-Path $work $Name
    $null = New-Item -ItemType Directory -Path $directory
    [IO.File]::WriteAllBytes((Join-Path $directory 'LICENSE'), $licenseBytes)
    # A rejected install must not reach device shutdown or shell replacement.
    [IO.File]::WriteAllText((Join-Path $directory 'gesher-app.cmd'), '@echo changed>"%~dp0shutdown.called"')
    [IO.File]::WriteAllText((Join-Path $directory 'gesher_shellext.dll'), 'untouched fixture')
    if ($Receipt) { [IO.File]::WriteAllText((Join-Path $directory 'gesher-license.ini'), "[License]`r`nSHA256=$Receipt`r`n") }
    $directory
}
function Change-Target([IntPtr] $Window, $Text, [string] $Old, [string] $New) {
    for ($page = 0; $page -lt 8; $page++) {
        $edits = @([Gesher.LicenseTest.Windows]::Children($Window) | Where-Object { $_.ClassName -match 'Edit$' -and $_.Text -ceq $Old })
        if ($edits.Count -eq 1) {
            [Gesher.LicenseTest.Windows]::SetText($edits[0].Handle, $New)
            Assert-License ([Gesher.LicenseTest.Windows]::Text($edits[0].Handle) -ceq $New) 'Directory change did not reach the installer'
            [Gesher.LicenseTest.Windows]::Click((Find-Control $Window $Text.ButtonNext).Handle)
            $null = Wait-License {
                Confirm-DirectoryDialog $Window $Text
                @(Read-License $Window).Count -eq 1
            } 'Changing to an unaccepted target skipped the agreement'
            return
        }
        $back = Find-Control $Window $Text.ButtonBack
        $before = ([Gesher.LicenseTest.Windows]::Children($Window) | ForEach-Object { "$($_.Handle):$($_.Text)" }) -join "`n"
        [Gesher.LicenseTest.Windows]::Click($back.Handle)
        $null = Wait-License {
            $after = ([Gesher.LicenseTest.Windows]::Children($Window) | ForEach-Object { "$($_.Handle):$($_.Text)" }) -join "`n"
            $after -cne $before
        } 'Setup did not return to its previous page'
    }
    throw 'Portable Setup did not return to its directory page'
}

try {
    if ($BuildFixture) {
        if ($InstallerPath) { throw 'Choose -BuildFixture or -InstallerPath, not both.' }
        $manifest = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../tools/package/gesher.iss'))
        $payload = Join-Path $work 'inert-payload'
        $null = New-Item -ItemType Directory -Path $payload
        foreach ($line in [IO.File]::ReadAllLines($manifest)) {
            if ($line -match '^Source: "\{#BuildDir\}\\([^"]+)"') {
                $path = Join-Path $payload $Matches[1].Replace('*', 'fixture')
                $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
                [IO.File]::WriteAllText($path, 'Inert license-wizard test payload; never execute.')
            }
        }
        & (Join-Path $InnoDirectory 'ISCC.exe') /Qp "/DBuildDir=$payload" /DAppVersion=0.0.0.1 "/O$work" $manifest
        if ($LASTEXITCODE -ne 0) { throw "Production installer compilation failed: $LASTEXITCODE" }
        $InstallerPath = Join-Path $work 'gesher-setup-0.0.0.1.exe'
    }
    if (-not $InstallerPath) { throw 'Pass -InstallerPath or -BuildFixture.' }
    $InstallerPath = (Resolve-Path -LiteralPath $InstallerPath).ProviderPath
    # Valid but different terms, not merely a malformed digest.
    $staleHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData(
        [Text.Encoding]::UTF8.GetBytes($licenseText + "`nDifferent agreement terms"))).ToLowerInvariant()
    foreach ($case in @(
        @{ Name = 'silent-missing'; Mode = '/SILENT'; Flag = @(); Receipt = '' },
        @{ Name = 'very-silent-missing'; Mode = '/VERYSILENT'; Flag = @(); Receipt = '' },
        @{ Name = 'silent-bare'; Mode = '/VERYSILENT'; Flag = @('/ACCEPTLICENSE'); Receipt = '' },
        @{ Name = 'silent-wrong'; Mode = '/VERYSILENT'; Flag = @('/ACCEPTLICENSE=wrong'); Receipt = '' },
        @{ Name = 'silent-stale-flag'; Mode = '/VERYSILENT'; Flag = @("/ACCEPTLICENSE=$staleHash"); Receipt = '' },
        @{ Name = 'silent-stale-receipt'; Mode = '/VERYSILENT'; Flag = @('/PORTABLE'); Receipt = $staleHash },
        @{ Name = 'silent-corrupt-receipt'; Mode = '/VERYSILENT'; Flag = @('/PORTABLE'); Receipt = 'not-a-digest' }
    )) {
        $directory = Fixture $case.Name $case.Receipt
        $before = Snapshot $directory
        $process = Start-Installer $directory 'en' (@($case.Mode) + $case.Flag)
        Assert-License ($process.WaitForExit(45000)) "$($case.Name): unattended Setup waited for UI"
        Assert-License ($process.ExitCode -ne 0) "$($case.Name): Setup accepted absent or mismatched consent"
        Assert-License ((Snapshot $directory) -ceq $before) "$($case.Name): rejection changed installation files or stopped the device"
        $process.Dispose(); $script:running = $null
    }

    foreach ($case in @(
        @{ Name = 'fresh-en'; Language = 'en'; Receipt = ''; Extra = @("/ACCEPTLICENSE=$licenseHash"); Skip = $false },
        @{ Name = 'changed-he'; Language = 'he'; Receipt = $staleHash; Extra = @(); Skip = $false },
        @{ Name = 'corrupt-en'; Language = 'en'; Receipt = 'invalid'; Extra = @(); Skip = $false },
        @{ Name = 'same-terms-en'; Language = 'en'; Receipt = $licenseHash; Extra = @(); Skip = $true },
        @{ Name = 'same-terms-portable'; Language = 'he'; Receipt = $licenseHash; Extra = @('/PORTABLE'); Skip = $true },
        @{ Name = 'fresh-portable'; Language = 'en'; Receipt = ''; Extra = @('/PORTABLE'); Skip = $false }
    )) {
        $directory = Fixture $case.Name $case.Receipt
        $before = Snapshot $directory
        $text = Messages $case.Language
        $process = Start-Installer $directory $case.Language $case.Extra
        $window = Window $process
        $state = Advance-ToAgreementOrReady $window $text
        if ($case.Skip) {
            Assert-License ($state -ceq 'ready') "$($case.Name): unchanged accepted terms asked for consent again"
        } else {
            Assert-License ($state -ceq 'agreement') "$($case.Name): new or changed terms were skipped"
            Check-Agreement $window $text
        }
        if ($case.Extra -contains '/PORTABLE') {
            if (-not $case.Skip) {
                [Gesher.LicenseTest.Windows]::Click((Find-Control $window $text.LicenseAccepted).Handle)
                $null = Wait-License { (Find-Control $window $text.ButtonNext).Enabled } 'Accept did not enable Next'
                [Gesher.LicenseTest.Windows]::Click((Find-Control $window $text.ButtonNext).Handle)
                $null = Wait-License { @(Read-License $window).Count -eq 0 } 'Agreement page did not close'
                Assert-License ((Advance-ToAgreementOrReady $window $text) -ceq 'ready') 'Accepted portable Setup did not reach Ready'
            }
            $other = Join-Path $work ($case.Name + '-other')
            Change-Target $window $text $directory $other
            Check-Agreement $window $text
            Assert-License (-not (Test-Path -LiteralPath $other)) 'A directory change wrote consent before installation'
        }
        Cancel-Installer $process $window $text
        Assert-License ((Snapshot $directory) -ceq $before) "$($case.Name): cancelling wrote consent or changed the installation"
    }
    "PASS: packaged installer license checks ($script:checks checks; full text en/he, explicit consent, changed terms, target changes and silent rejection)"
} catch {
    $latestLog = Join-Path $work ($script:sequence.ToString() + '.log')
    if (Test-Path -LiteralPath $latestLog) { Get-Content -LiteralPath $latestLog -Tail 60 | Write-Host }
    throw
} finally {
    if ($script:running -and -not $script:running.HasExited) {
        # Only this invocation and its children; never close another setup.
        & taskkill.exe /PID $script:running.Id /T /F | Out-Null
        $null = $script:running.WaitForExit(10000)
    }
    if ($script:running) { $script:running.Dispose() }
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}
