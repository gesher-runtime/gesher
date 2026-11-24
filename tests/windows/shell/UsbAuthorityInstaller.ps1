param(
    [Parameter(Mandatory = $true)]
    [string]$Setup,
    [Parameter(Mandatory = $true)]
    [string]$BrokerImage,
    [string]$CardWakeApk
)

$ErrorActionPreference = 'Stop'
$service = 'GesherUsbAuthority'
$runtimeService = 'GesherProtectedRuntime'
$folder = Join-Path $env:ProgramFiles 'Gesher Raw USB Broker'
$exe = Join-Path $folder 'gesher-usb-authority.exe'
$uninstall = Join-Path $folder 'unins000.exe'
$ordinary = Join-Path $env:LOCALAPPDATA 'Programs\Gesher'
$ordinaryExisted = Test-Path -LiteralPath $ordinary
$privateRoot = Join-Path $env:ProgramData 'Gesher Protected Android'
$privateRootExisted = Test-Path -LiteralPath $privateRoot
$fixtureRoot = Join-Path $env:ProgramFiles ('gesher-identity-' + [guid]::NewGuid())
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'The disposable installer test requires administrative rights.'
}
if ((Get-Service -Name $service -ErrorAction SilentlyContinue) -or
    (Get-Service -Name $runtimeService -ErrorAction SilentlyContinue) -or
    (Test-Path -LiteralPath $folder)) {
    throw 'The disposable installer test requires an empty machine installation.'
}

function Invoke-Setup([string]$Image) {
    $process = Start-Process -FilePath $Image -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' `
        -PassThru -Wait -WindowStyle Hidden
    if ($process.ExitCode -ne 0) { throw "$Image returned $($process.ExitCode)" }
}

$trustedOwners = @('S-1-5-18', 'S-1-5-32-544',
    'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')
function Assert-ProtectedOwner([string]$Path) {
    $sid = (Get-Acl -LiteralPath $Path).GetOwner([Security.Principal.SecurityIdentifier]).Value
    if ($sid -notin $trustedOwners) { throw "Unprotected owner on $Path" }
}

function Read-ServiceAcl([string]$Name = $service) {
    $sddl = @(& sc.exe sdshow $Name | Where-Object { $_ -match '^[OGDS]:' }) -join ''
    if ($LASTEXITCODE -ne 0 -or -not $sddl) { throw 'Could not read the optional service ACL.' }
    [Security.AccessControl.RawSecurityDescriptor]::new($sddl)
}

# The negative probes stay in this process with a filtered copy of the real
# desktop token. A separate disposable LocalSystem task below queries the
# private service token, matching the production authority's caller. Debug
# privilege on a desktop process does not bypass the private token's DACL.
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Principal;
public static class GesherIdentityProbe {
    [StructLayout(LayoutKind.Sequential)] struct SidAndAttributes { public IntPtr Sid; public uint Attributes; }
    [StructLayout(LayoutKind.Sequential)] struct SecurityAttributes { public int Size; public IntPtr Descriptor; public int Inherit; }
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool CreateRestrictedToken(IntPtr token, uint flags, uint disableCount, [In] SidAndAttributes[] disabled, uint deleteCount, IntPtr deleted, uint restrictedCount, IntPtr restricted, out IntPtr result);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool ImpersonateLoggedOnUser(IntPtr token);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool RevertToSelf();
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr CreateFile(string path, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", EntryPoint="CreateFileW", ExactSpelling=true, CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr CreateProtectedFile(string path, uint access, uint share, ref SecurityAttributes security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool CreateDirectory(string path, ref SecurityAttributes security);
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool ConvertStringSecurityDescriptorToSecurityDescriptor(string text, uint revision, out IntPtr descriptor, IntPtr size);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr memory);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool WriteFile(IntPtr file, byte[] bytes, uint count, out uint written, IntPtr overlapped);
    [DllImport("wtsapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool WTSQuerySessionInformation(IntPtr server, uint session, int info, out IntPtr value, out uint size);
    [DllImport("wtsapi32.dll")] static extern void WTSFreeMemory(IntPtr value);
    static IntPtr desktopToken;
    static void Require(bool result, string operation) {
        if (!result) {
            int error = Marshal.GetLastWin32Error();
            throw new Win32Exception(error, operation + " failed with Win32 " + error);
        }
    }
    static SecurityAttributes ProbeSecurity(bool directory) {
        IntPtr descriptor;
        string flags = directory ? "OICI" : "";
        Require(ConvertStringSecurityDescriptorToSecurityDescriptor(
            "O:BAG:BAD:P(A;" + flags + ";FA;;;SY)(A;" + flags + ";FA;;;BA)",
            1, out descriptor, IntPtr.Zero), "ConvertStringSecurityDescriptorToSecurityDescriptor(test fixture)");
        return new SecurityAttributes { Size=Marshal.SizeOf(typeof(SecurityAttributes)), Descriptor=descriptor, Inherit=0 };
    }
    public static void CreatePrivateDirectory(string path) {
        var security = ProbeSecurity(true);
        try { Require(CreateDirectory(path, ref security), "CreateDirectory(protected test ancestor)"); }
        finally { LocalFree(security.Descriptor); }
    }
    public static void WritePrivateFile(string path, byte[] content) {
        var security = ProbeSecurity(false);
        IntPtr file;
        try {
            file = CreateProtectedFile(path, 0x40000000, 0, ref security, 1, 0, IntPtr.Zero);
            Require(file != new IntPtr(-1), "CreateFile(protected test helper/request, GENERIC_WRITE|CREATE_NEW)");
        } finally { LocalFree(security.Descriptor); }
        try {
            uint written;
            Require(WriteFile(file, content, (uint)content.Length, out written, IntPtr.Zero), "WriteFile(protected test helper/request)");
            if (written != content.Length) throw new Win32Exception(29, "Short protected test file write");
        } finally { CloseHandle(file); }
    }
    public static void InitializeDesktop() {
        IntPtr self;
        Require(OpenProcessToken(GetCurrentProcess(), 0xA, out self),
            "OpenProcessToken(desktop, TOKEN_QUERY|TOKEN_DUPLICATE=0xA)");
        try {
            var admin = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
            var bytes = new byte[admin.BinaryLength]; admin.GetBinaryForm(bytes, 0);
            var sid = Marshal.AllocHGlobal(bytes.Length);
            try {
                Marshal.Copy(bytes, 0, sid, bytes.Length);
                Require(CreateRestrictedToken(self, 2, 1,
                    new[] { new SidAndAttributes { Sid=sid, Attributes=0 } },
                    0, IntPtr.Zero, 0, IntPtr.Zero, out desktopToken),
                    "CreateRestrictedToken(desktop, DISABLE_MAX_PRIVILEGE, Administrators deny-only)");
            } finally { Marshal.FreeHGlobal(sid); }
        } finally { CloseHandle(self); }
    }
    public static int OpenAsDesktop(string path, uint access) {
        Require(ImpersonateLoggedOnUser(desktopToken), "ImpersonateLoggedOnUser(filtered desktop)");
        try {
            IntPtr file = CreateFile(path, access, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero);
            if (file == new IntPtr(-1)) return Marshal.GetLastWin32Error();
            CloseHandle(file); return 0;
        } finally { if (!RevertToSelf()) Environment.FailFast("RevertToSelf(filtered desktop) failed"); }
    }
    public static int CreateProbe(string path) {
        Require(ImpersonateLoggedOnUser(desktopToken), "ImpersonateLoggedOnUser(filtered desktop)");
        try {
            IntPtr file = CreateFile(path, 0x40000000, 7, IntPtr.Zero, 1, 0, IntPtr.Zero);
            if (file == new IntPtr(-1)) return Marshal.GetLastWin32Error();
            try {
                uint written;
                if (!WriteFile(file, new byte[] { 71, 83, 72 }, 3, out written, IntPtr.Zero)) return Marshal.GetLastWin32Error();
                return written == 3 ? 0 : 29;
            } finally { CloseHandle(file); }
        } finally { if (!RevertToSelf()) Environment.FailFast("RevertToSelf(filtered desktop) failed"); }
    }
    public static bool InteractiveSession(uint session) {
        if (session == 0) return false;
        IntPtr value; uint size;
        if (!WTSQuerySessionInformation(IntPtr.Zero, session, 8, out value, out size)) return false;
        bool active;
        try { active = size == 4 && Marshal.ReadInt32(value) == 0; }
        finally { WTSFreeMemory(value); }
        if (!active || !WTSQuerySessionInformation(IntPtr.Zero, session, 5, out value, out size)) return false;
        try { return size > 2 && !String.IsNullOrEmpty(Marshal.PtrToStringUni(value)); }
        finally { WTSFreeMemory(value); }
    }
    public static void Dispose() {
        if (desktopToken != IntPtr.Zero) CloseHandle(desktopToken);
        desktopToken = IntPtr.Zero;
    }
}
'@

function New-ProtectedProbeDirectory([string]$Path) {
    [GesherIdentityProbe]::CreatePrivateDirectory($Path)
}

function Invoke-SystemIdentityProbe([string]$Operation, [uint32]$HolderProcessId,
    [string]$FixturePath = '', [uint32[]]$GuestProcessIds = @()) {
    $probeName = 'GesherIdentityProbe-' + [guid]::NewGuid().ToString('N')
    # Program Files prevents an ordinary desktop process replacing an ancestor
    # while the SYSTEM task starts. The copied helper, request and result have
    # only SYSTEM/Administrators grants and trusted owners.
    $probeDirectory = Join-Path $env:ProgramFiles $probeName
    $nonce = [guid]::NewGuid().ToString('N')
    $registered = $false
    try {
        New-ProtectedProbeDirectory $probeDirectory
        $helperPath = Join-Path $probeDirectory 'probe.ps1'
        [GesherIdentityProbe]::WritePrivateFile($helperPath,
            [IO.File]::ReadAllBytes((Join-Path $PSScriptRoot 'UsbAuthorityIdentityProbe.ps1')))
        $requestPath = Join-Path $probeDirectory 'request.json'
        $resultPath = Join-Path $probeDirectory 'result.json'
        $requestBody = @{
            Nonce = $nonce
            Operation = $Operation
            HolderProcessId = $HolderProcessId
            FixturePath = $FixturePath
            GuestProcessIds = @($GuestProcessIds)
        } | ConvertTo-Json -Depth 4
        [GesherIdentityProbe]::WritePrivateFile($requestPath, [Text.Encoding]::UTF8.GetBytes($requestBody))
        foreach ($path in $probeDirectory, $helperPath, $requestPath) {
            Assert-ProtectedOwner $path
            Assert-DesktopDenied $path
        }
        # Register the SYSTEM task with its final owner/DACL atomically. A
        # desktop-owned default task followed by an ACL repair leaves a gap.
        $scheduler = New-Object -ComObject 'Schedule.Service'
        $scheduler.Connect()
        $taskFolder = $scheduler.GetFolder('\')
        $definition = $scheduler.NewTask(0)
        $definition.Principal.UserId = 'SYSTEM'
        $definition.Principal.LogonType = 5 # TASK_LOGON_SERVICE_ACCOUNT
        $definition.Principal.RunLevel = 1 # TASK_RUNLEVEL_HIGHEST
        $definition.Settings.ExecutionTimeLimit = 'PT60S'
        $definition.Settings.DisallowStartIfOnBatteries = $false
        $definition.Settings.StopIfGoingOnBatteries = $false
        $action = $definition.Actions.Create(0) # TASK_ACTION_EXEC
        $action.Path = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
        $action.Arguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + $helperPath + '" -Request "' + $requestPath + '"'
        $task = $taskFolder.RegisterTaskDefinition($probeName, $definition, 0x12, 'SYSTEM', $null, 5,
            'O:BAG:BAD:P(A;;GA;;;SY)(A;;GA;;;BA)') # TASK_CREATE | TASK_DONT_ADD_PRINCIPAL_ACE
        $registered = $true
        $taskSecurity = [Security.AccessControl.RawSecurityDescriptor]::new($task.GetSecurityDescriptor(5))
        if ($taskSecurity.Owner.Value -ne 'S-1-5-32-544' -or -not $taskSecurity.DiscretionaryAcl -or
            $taskSecurity.DiscretionaryAcl.Count -ne 2) { throw 'The disposable SYSTEM task has an unprotected owner or ACL.' }
        foreach ($ace in $taskSecurity.DiscretionaryAcl) {
            if ($ace.AceType -ne [Security.AccessControl.AceType]::AccessAllowed -or
                $ace.SecurityIdentifier.Value -notin 'S-1-5-18', 'S-1-5-32-544') {
                throw 'The disposable SYSTEM task has an untrusted grant.'
            }
        }
        $previousRun = (Get-ScheduledTaskInfo -TaskName $probeName).LastRunTime
        Start-ScheduledTask -TaskName $probeName
        $deadline = [DateTime]::UtcNow.AddSeconds(75)
        do {
            $state = (Get-ScheduledTask -TaskName $probeName).State
            $info = Get-ScheduledTaskInfo -TaskName $probeName
            if ((Test-Path -LiteralPath $resultPath) -and $state -notin 'Running', 'Queued' -and
                $info.LastRunTime -gt $previousRun -and $info.LastTaskResult -ne 267009) { break }
            if ([DateTime]::UtcNow -ge $deadline) {
                $taskResult = (Get-ScheduledTaskInfo -TaskName $probeName).LastTaskResult
                throw "The LocalSystem identity probe timed out (task state=$state, result=$taskResult)."
            }
            Start-Sleep -Milliseconds 100
        } while ($true)
        $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
        $taskResult = (Get-ScheduledTaskInfo -TaskName $probeName).LastTaskResult
        if ($result.Nonce -cne $nonce -or -not $result.Success -or $taskResult -ne 0) {
            throw "LocalSystem identity probe failed (task result=$taskResult): $($result.Error)"
        }
        Write-Output $result
    } finally {
        try {
            if ($registered) {
                try {
                    Stop-ScheduledTask -TaskName $probeName -ErrorAction SilentlyContinue
                    $stopDeadline = [DateTime]::UtcNow.AddSeconds(10)
                    while ((Get-ScheduledTask -TaskName $probeName).State -in 'Running', 'Queued') {
                        if ([DateTime]::UtcNow -ge $stopDeadline) { throw 'The disposable identity probe did not stop.' }
                        Start-Sleep -Milliseconds 100
                    }
                } finally {
                    Unregister-ScheduledTask -TaskName $probeName -Confirm:$false
                    if (Get-ScheduledTask -TaskName $probeName -ErrorAction SilentlyContinue) {
                        throw 'The disposable LocalSystem identity-probe task was not removed.'
                    }
                }
            }
        } finally {
            if (Test-Path -LiteralPath $probeDirectory) { Remove-Item -LiteralPath $probeDirectory -Recurse -Force }
        }
    }
}

function Assert-DesktopDenied([string]$Path) {
    foreach ($right in [uint32]0x40000000, [uint32]0x00040000, [uint32]0x00010000) {
        $errorCode = [GesherIdentityProbe]::OpenAsDesktop($Path, $right)
        if ($errorCode -ne 5) { throw "Desktop token access $right to $Path returned $errorCode; expected ACCESS_DENIED." }
    }
}

function Stop-Authority {
    Stop-Service -Name $service
    $controller = Get-Service -Name $service
    try {
        $controller.WaitForStatus([ServiceProcess.ServiceControllerStatus]::Stopped, [TimeSpan]::FromSeconds(30))
    } finally { $controller.Dispose() }
}

function Read-SharedText([string]$Path) {
    $stream = [IO.FileStream]::new($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader = [IO.StreamReader]::new($stream)
    try { $reader.ReadToEnd() } finally { $reader.Dispose() }
}

$setupPath = [IO.Path]::GetFullPath($Setup)
$outside = Start-Process -FilePath ([IO.Path]::GetFullPath($BrokerImage)) `
    -ArgumentList 'install-service' -PassThru -Wait -WindowStyle Hidden
if ($outside.ExitCode -ne 1 -or (Get-Service -Name $service -ErrorAction SilentlyContinue)) {
    throw 'The USB authority accepted service registration from an unprotected binary location.'
}

$redirected = Join-Path ([IO.Path]::GetTempPath()) ("gesher-usb-invalid-" + [guid]::NewGuid())
$wrongDir = Start-Process -FilePath $setupPath -PassThru -Wait -WindowStyle Hidden `
    -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', ('/DIR="' + $redirected + '"')
if ($wrongDir.ExitCode -eq 0 -or (Test-Path -LiteralPath $redirected) -or
    (Test-Path -LiteralPath $folder) -or (Get-Service -Name $service -ErrorAction SilentlyContinue)) {
    throw 'The installer accepted a redirected service destination.'
}

try {
    Invoke-Setup $setupPath
    $configuration = Get-CimInstance Win32_Service -Filter "Name='$service'"
    if (-not $configuration -or $configuration.PathName -ine ('"' + $exe + '" service') -or
        $configuration.StartMode -ne 'Manual' -or $configuration.StartName -ne 'LocalSystem' -or
        $configuration.State -ne 'Stopped') {
        throw "Unexpected initial configuration for $service"
    }
    $runtimeConfiguration = Get-CimInstance Win32_Service -Filter "Name='$runtimeService'"
    if (-not $runtimeConfiguration -or
        $runtimeConfiguration.PathName -ine ('"' + $exe + '" runtime-identity') -or
        $runtimeConfiguration.StartMode -ne 'Manual' -or
        $runtimeConfiguration.StartName -ine ('NT SERVICE\' + $runtimeService) -or
        $runtimeConfiguration.State -ne 'Stopped') {
        throw "Unexpected private account configuration for $runtimeService"
    }
    $runtimeAcl = Read-ServiceAcl $runtimeService
    foreach ($ace in $runtimeAcl.DiscretionaryAcl) {
        if ($ace.AceType -ne [Security.AccessControl.AceType]::AccessAllowed -or
            $ace.SecurityIdentifier.Value -notin @('S-1-5-18', 'S-1-5-32-544')) {
            throw 'The private account service has a non-administrative grant.'
        }
    }
    Assert-ProtectedOwner $folder
    Assert-ProtectedOwner $exe
    Assert-ProtectedOwner $uninstall
    foreach ($relative in 'dalvikvm.exe', 'binderd.exe', 'servicemanager.exe',
        'framework\x86_64\boot.art', 'system\framework\framework-res.apk',
        'libgesher_webview_bridge.so') {
        Assert-ProtectedOwner (Join-Path $folder "runtime\$relative")
    }
    if (Test-Path -LiteralPath (Join-Path $folder 'runtime\data')) {
        throw 'The runtime tree contains mutable Android data.'
    }
    $descriptor = Read-ServiceAcl
    $queryAccess = $false
    foreach ($ace in $descriptor.DiscretionaryAcl) {
        if ($ace.AceType -ne [Security.AccessControl.AceType]::AccessAllowed) { continue }
        if ($ace.SecurityIdentifier.Value -notin $trustedOwners -and ($ace.AccessMask -band 0x520D0172)) {
            throw 'An untrusted principal can modify or control the optional service.'
        }
        if ($ace.SecurityIdentifier.Value -eq 'S-1-5-11' -and ($ace.AccessMask -band 0x5) -eq 0x5) {
            $queryAccess = $true
        }
    }
    if (-not $queryAccess) { throw 'Authenticated users cannot query the optional service.' }
    # Represent a query-only AppContainer ACE left behind by an interrupted
    # bootstrap. Starting the authority must retire this exact ACE.
    $stalePackage = [Security.Principal.SecurityIdentifier]::new('S-1-15-2-400-401-402-403-404-405-406')
    $staleAce = [Security.AccessControl.CommonAce]::new(
        [Security.AccessControl.AceFlags]::None,
        [Security.AccessControl.AceQualifier]::AccessAllowed, 0x5, $stalePackage, $false, $null)
    $descriptor.DiscretionaryAcl.InsertAce($descriptor.DiscretionaryAcl.Count, $staleAce)
    $testAcl = $descriptor.GetSddlForm([Security.AccessControl.AccessControlSections]::Access)
    & sc.exe sdset $service $testAcl | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Could not set the isolated interrupted-bootstrap test ACL.' }
    Start-Service -Name $service
    $controller = Get-Service -Name $service
    try {
        $controller.WaitForStatus(
            [ServiceProcess.ServiceControllerStatus]::Running, [TimeSpan]::FromSeconds(20))
    } finally {
        $controller.Dispose()
    }
    $after = Read-ServiceAcl
    if ($after.DiscretionaryAcl | Where-Object { $_.SecurityIdentifier -eq $stalePackage }) {
        throw 'Service startup did not clear the stale bootstrap query grant.'
    }
    Start-Service -Name $runtimeService
    $holder = Get-Service -Name $runtimeService
    try {
        $holder.WaitForStatus([ServiceProcess.ServiceControllerStatus]::Running, [TimeSpan]::FromSeconds(20))
    } finally { $holder.Dispose() }
    $runtimeConfiguration = Get-CimInstance Win32_Service -Filter "Name='$runtimeService'"
    [GesherIdentityProbe]::InitializeDesktop()
    $expectedSid = ([Security.Principal.NTAccount]::new('NT SERVICE', $runtimeService)).Translate(
        [Security.Principal.SecurityIdentifier]).Value
    Assert-DesktopDenied $folder
    Assert-DesktopDenied $exe
    Assert-DesktopDenied (Join-Path $folder 'runtime\dalvikvm.exe')

    # The protected ancestor also denies desktop DELETE_CHILD. A fixture under
    # the desktop user's own temp directory would not prove deletion denial.
    New-ProtectedProbeDirectory $fixtureRoot
    $positive = Join-Path $fixtureRoot 'desktop-control'
    New-Item -ItemType Directory -Path $positive | Out-Null
    $acl = Get-Acl -LiteralPath $positive
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
        $identity.User, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow'))
    Set-Acl -LiteralPath $positive -AclObject $acl
    $controlError = [GesherIdentityProbe]::CreateProbe((Join-Path $positive 'written.bin'))
    if ($controlError -ne 0) { throw "Filtered desktop token control write failed: $controlError" }
    $privateFixture = Join-Path $fixtureRoot 'private-account'
    New-Item -ItemType Directory -Path $privateFixture | Out-Null
    $acl = [Security.AccessControl.DirectorySecurity]::new()
    $acl.SetAccessRuleProtection($true, $false)
    $acl.SetOwner([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))
    foreach ($sid in 'S-1-5-18', 'S-1-5-32-544', $expectedSid) {
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
            [Security.Principal.SecurityIdentifier]::new($sid), 'FullControl',
            'ContainerInherit,ObjectInherit', 'None', 'Allow'))
    }
    Set-Acl -LiteralPath $privateFixture -AclObject $acl
    Assert-DesktopDenied $privateFixture
    $privateFile = Join-Path $privateFixture 'runtime-write.bin'
    if ([GesherIdentityProbe]::CreateProbe($privateFile) -ne 5) {
        throw 'An ordinary desktop token could create a file in the private-account fixture.'
    }
    $runtimeProbe = Invoke-SystemIdentityProbe 'WriteRuntimeFixture' ([uint32]$runtimeConfiguration.ProcessId) $privateFile
    $runtimeSid = $runtimeProbe.RuntimeSid
    if ($runtimeSid -ne $expectedSid -or $runtimeSid -eq $identity.User.Value) {
        throw 'The SCM runtime process does not have its distinct private TokenUser.'
    }
    if ($runtimeProbe.WriteError -ne 0 -or [IO.File]::ReadAllText($privateFile) -ne 'GSH') {
        throw "The real private SCM token could not write its NTFS fixture: $($runtimeProbe.WriteError)"
    }
    Assert-DesktopDenied $privateFile
    Write-Output 'PASS: actual SCM virtual TokenUser, protected installation ACLs, desktop denial and private-token write in isolated NTFS fixture'

    if ($CardWakeApk) {
        $apk = [IO.Path]::GetFullPath($CardWakeApk)
        $expectedHash = 'd0afc9864ab0dd1305537cc0f7e2e8e127c11a31cecb012a1e18e210eaf2b313'
        if ((Get-FileHash -LiteralPath $apk -Algorithm SHA256).Hash -ine $expectedHash) {
            throw 'The original CardWake APK does not match the pinned release asset.'
        }
        $session = [uint32][Diagnostics.Process]::GetCurrentProcess().SessionId
        if (-not [GesherIdentityProbe]::InteractiveSession($session)) {
            Write-Output "LIMITATION: no active WTS desktop for this Windows runner (session $session); original APK activity/window/WebView boot was not executed. Identity and ACL probes above were executed."
        } else {
            if ($privateRootExisted) { throw 'Original APK boot requires an empty disposable protected-data root.' }
            Stop-Authority
            $launch = Start-Process -FilePath $exe -ArgumentList ('launch-apk-no-media "' + $apk + '"') -PassThru
            try {
                if (-not $launch.WaitForExit(120000)) {
                    $launch.Kill()
                    throw 'The elevated original-APK launch did not finish within its bounded readiness interval.'
                }
                if ($launch.ExitCode -ne 0) { throw "Protected original APK native startup failed with Win32 status $($launch.ExitCode)." }
            } finally { $launch.Dispose() }
            $roots = @(Get-ChildItem -LiteralPath $privateRoot -Directory)
            if ($roots.Count -ne 1) { throw 'Protected startup did not produce one desktop-private Android root.' }
            $androidRoot = $roots[0].FullName
            foreach ($relative in '', 'data', 'stage', 'tmp', 'tmp\server', 'tmp\com.cardwake.otgroot') {
                $path = if ($relative) { Join-Path $androidRoot $relative } else { $androidRoot }
                Assert-ProtectedOwner $path
                Assert-DesktopDenied $path
            }
            $staged = @(Get-ChildItem -LiteralPath (Join-Path $androidRoot 'stage') -Recurse -Filter '*.apk' -File)
            if ($staged.Count -ne 1 -or $staged[0].Name -ne 'base.apk' -or
                $staged[0].Directory.Name -ine $expectedHash -or
                (Get-FileHash -LiteralPath $staged[0].FullName -Algorithm SHA256).Hash -ine $expectedHash) {
                throw 'The authority did not retain the original exact APK in protected staging.'
            }
            Assert-DesktopDenied $staged[0].FullName
            $guestProcesses = @(Get-CimInstance Win32_Process | Where-Object {
                $_.ExecutablePath -ieq (Join-Path $folder 'runtime\dalvikvm.exe')
            })
            if ($guestProcesses.Count -lt 2) { throw 'Protected startup did not retain both system_server and the original app.' }
            $guestIds = @($guestProcesses | ForEach-Object { [uint32]$_.ProcessId })
            $guestProbe = Invoke-SystemIdentityProbe 'QueryGuestUsers' ([uint32]$runtimeConfiguration.ProcessId) '' $guestIds
            if ($guestProbe.RuntimeSid -ne $runtimeSid -or @($guestProbe.Guests).Count -ne $guestIds.Count) {
                throw 'The protected guest identity probe did not retain the expected private account and process set.'
            }
            foreach ($guest in $guestProbe.Guests) {
                if ($guest.UserSid -ne $runtimeSid -or [uint32]$guest.ProcessId -notin $guestIds) {
                    throw 'A protected Android guest is running under the desktop account.'
                }
            }
            $logDirectory = Join-Path $androidRoot 'logs'
            $deadline = [DateTime]::UtcNow.AddSeconds(90)
            $windowShown = $false
            $webViewLoaded = $false
            do {
                $appLog = @('com.cardwake.otgroot.log', 'com.cardwake.otgroot.logcat') |
                    ForEach-Object {
                        $path = Join-Path $logDirectory $_
                        if (Test-Path -LiteralPath $path) { Read-SharedText $path }
                    }
                $body = $appLog -join "`n"
                if ($body -match 'FATAL EXCEPTION|WebView.*(?:unavailable|not available)|Unavailable WebView') {
                    throw 'The original APK reported a fatal error or unavailable WebView.'
                }
                $windowShown = $body -match 'first frame drawn \([1-9][0-9]* presented\), shown, hwnd 0x[1-9a-fA-F][0-9a-fA-F]*'
                $webViewLoaded = $body -match 'first page loaded: https://appassets\.androidplatform\.net/assets/www/index\.html'
                if ($windowShown -and $webViewLoaded) { break }
                if ((Get-Service -Name $service).Status -ne 'Running') { throw 'The protected authority exited before the original APK rendered.' }
                Start-Sleep -Milliseconds 200
            } while ([DateTime]::UtcNow -lt $deadline)
            if (-not $windowShown -or -not $webViewLoaded) {
                throw "Original CardWake UI readiness failed: shown frame=$windowShown, original asset WebView=$webViewLoaded."
            }
            if ((Get-FileHash -LiteralPath $apk -Algorithm SHA256).Hash -ine $expectedHash) { throw 'The original APK changed during execution.' }
            Write-Output 'PASS: original pinned CardWake APK, protected native boot, real desktop frame and original WebView asset load; production Android data rejects ordinary desktop writes'
            Write-Output 'LIMITATION: no storage grant was issued; physical reader/card, media write, disconnect, card replacement and real-card revocation were not exercised.'
        }
    } else {
        Write-Output 'LIMITATION: no original CardWake APK was supplied; protected ART/activity/WebView startup was not exercised.'
    }
    # Exercise the uninstall stop path with the service still running.
} finally {
    if (Test-Path -LiteralPath $privateRoot) {
        Get-ChildItem -LiteralPath $privateRoot -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Extension -in '.log', '.logcat' } | ForEach-Object {
                Write-Output ("--- protected runtime evidence: " + $_.FullName)
                Get-Content -LiteralPath $_.FullName -Tail 150 -ErrorAction SilentlyContinue
            }
    }
    [GesherIdentityProbe]::Dispose()
    if (Test-Path -LiteralPath $uninstall -PathType Leaf) {
        Invoke-Setup $uninstall
    }
    if (Test-Path -LiteralPath $fixtureRoot) { Remove-Item -LiteralPath $fixtureRoot -Recurse -Force }
    if (-not $privateRootExisted -and (Test-Path -LiteralPath $privateRoot)) {
        Remove-Item -LiteralPath $privateRoot -Recurse -Force
    }
}
$deadline = [DateTime]::UtcNow.AddSeconds(20)
while ($existing = Get-Service -Name $service -ErrorAction SilentlyContinue) {
    $existing.Dispose()
    if ([DateTime]::UtcNow -ge $deadline) { throw 'Uninstall left the optional service registered.' }
    Start-Sleep -Milliseconds 100
}
if (Get-Service -Name $runtimeService -ErrorAction SilentlyContinue) {
    throw 'Uninstall left the private runtime identity service registered.'
}
if (Test-Path -LiteralPath $exe) {
    throw 'Uninstall left the optional service or broker image behind.'
}
if ((Test-Path -LiteralPath $ordinary) -ne $ordinaryExisted) {
    throw 'The optional installer changed the ordinary installation.'
}
Write-Output 'PASS: optional USB authority installation, manual start, stop and uninstall'
