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
$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('gesher-identity-' + [guid]::NewGuid())
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

# These probes use the real SCM service token and a filtered copy of the test
# desktop account. No media, mocked device or guest security bypass is involved.
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Principal;
public static class GesherIdentityProbe {
    [StructLayout(LayoutKind.Sequential)] struct Luid { public uint Low; public int High; }
    [StructLayout(LayoutKind.Sequential)] struct Privilege { public uint Count; public Luid Luid; public uint Attributes; }
    [StructLayout(LayoutKind.Sequential)] struct SidAndAttributes { public IntPtr Sid; public uint Attributes; }
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool LookupPrivilegeValue(string system, string name, out Luid luid);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, ref Privilege privilege, uint length, IntPtr previous, IntPtr returned);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool CreateRestrictedToken(IntPtr token, uint flags, uint disableCount, [In] SidAndAttributes[] disabled, uint deleteCount, IntPtr deleted, uint restrictedCount, IntPtr restricted, out IntPtr result);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool ImpersonateLoggedOnUser(IntPtr token);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool RevertToSelf();
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr CreateFile(string path, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool WriteFile(IntPtr file, byte[] bytes, uint count, out uint written, IntPtr overlapped);
    [DllImport("wtsapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool WTSQuerySessionInformation(IntPtr server, uint session, int info, out IntPtr value, out uint size);
    [DllImport("wtsapi32.dll")] static extern void WTSFreeMemory(IntPtr value);
    static IntPtr desktopToken;
    static IntPtr runtimeToken;
    static void Require(bool result) { if (!result) throw new Win32Exception(Marshal.GetLastWin32Error()); }
    public static string Initialize(uint holderPid) {
        IntPtr self;
        Require(OpenProcessToken(GetCurrentProcess(), 0xF01FF, out self));
        try {
            Luid debug; Require(LookupPrivilegeValue(null, "SeDebugPrivilege", out debug));
            var privilege = new Privilege { Count=1, Luid=debug, Attributes=2 };
            Require(AdjustTokenPrivileges(self, false, ref privilege, 0, IntPtr.Zero, IntPtr.Zero));
            if (Marshal.GetLastWin32Error() != 0) throw new Win32Exception(Marshal.GetLastWin32Error());
            var admin = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
            var bytes = new byte[admin.BinaryLength]; admin.GetBinaryForm(bytes, 0);
            var sid = Marshal.AllocHGlobal(bytes.Length);
            try {
                Marshal.Copy(bytes, 0, sid, bytes.Length);
                Require(CreateRestrictedToken(self, 2, 1,
                    new[] { new SidAndAttributes { Sid=sid, Attributes=0 } },
                    0, IntPtr.Zero, 0, IntPtr.Zero, out desktopToken));
            } finally { Marshal.FreeHGlobal(sid); }
        } finally { CloseHandle(self); }
        IntPtr process = OpenProcess(0x1000, false, holderPid);
        if (process == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        try { Require(OpenProcessToken(process, 0xE, out runtimeToken)); }
        finally { CloseHandle(process); }
        using (var identity = new WindowsIdentity(runtimeToken)) { return identity.User.Value; }
    }
    public static string ProcessUser(uint pid) {
        IntPtr process = OpenProcess(0x1000, false, pid), token;
        if (process == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        try { Require(OpenProcessToken(process, 8, out token)); }
        finally { CloseHandle(process); }
        try { using (var identity = new WindowsIdentity(token)) { return identity.User.Value; } }
        finally { CloseHandle(token); }
    }
    public static int OpenAsDesktop(string path, uint access) {
        Require(ImpersonateLoggedOnUser(desktopToken));
        try {
            IntPtr file = CreateFile(path, access, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero);
            if (file == new IntPtr(-1)) return Marshal.GetLastWin32Error();
            CloseHandle(file); return 0;
        } finally { Require(RevertToSelf()); }
    }
    public static int CreateProbe(string path, bool runtime) {
        Require(ImpersonateLoggedOnUser(runtime ? runtimeToken : desktopToken));
        try {
            IntPtr file = CreateFile(path, 0x40000000, 7, IntPtr.Zero, 1, 0, IntPtr.Zero);
            if (file == new IntPtr(-1)) return Marshal.GetLastWin32Error();
            try {
                uint written;
                if (!WriteFile(file, new byte[] { 71, 83, 72 }, 3, out written, IntPtr.Zero)) return Marshal.GetLastWin32Error();
                return written == 3 ? 0 : 29;
            } finally { CloseHandle(file); }
        } finally { Require(RevertToSelf()); }
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
        if (runtimeToken != IntPtr.Zero) CloseHandle(runtimeToken);
        if (desktopToken != IntPtr.Zero) CloseHandle(desktopToken);
        runtimeToken = desktopToken = IntPtr.Zero;
    }
}
'@

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
        [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
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
    $runtimeSid = [GesherIdentityProbe]::Initialize([uint32]$runtimeConfiguration.ProcessId)
    $expectedSid = ([Security.Principal.NTAccount]::new('NT SERVICE', $runtimeService)).Translate(
        [Security.Principal.SecurityIdentifier]).Value
    if ($runtimeSid -ne $expectedSid -or $runtimeSid -eq $identity.User.Value) {
        throw 'The SCM runtime process does not have its distinct private TokenUser.'
    }
    Assert-DesktopDenied $folder
    Assert-DesktopDenied $exe
    Assert-DesktopDenied (Join-Path $folder 'runtime\dalvikvm.exe')

    New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
    $positive = Join-Path $fixtureRoot 'desktop-control'
    New-Item -ItemType Directory -Path $positive | Out-Null
    $acl = Get-Acl -LiteralPath $positive
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
        $identity.User, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow'))
    Set-Acl -LiteralPath $positive -AclObject $acl
    $controlError = [GesherIdentityProbe]::CreateProbe((Join-Path $positive 'written.bin'), $false)
    if ($controlError -ne 0) { throw "Filtered desktop token control write failed: $controlError" }
    $privateFixture = Join-Path $fixtureRoot 'private-account'
    New-Item -ItemType Directory -Path $privateFixture | Out-Null
    $acl = [Security.AccessControl.DirectorySecurity]::new()
    $acl.SetAccessRuleProtection($true, $false)
    $acl.SetOwner([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))
    foreach ($sid in 'S-1-5-18', 'S-1-5-32-544', $runtimeSid) {
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
            [Security.Principal.SecurityIdentifier]::new($sid), 'FullControl',
            'ContainerInherit,ObjectInherit', 'None', 'Allow'))
    }
    Set-Acl -LiteralPath $privateFixture -AclObject $acl
    Assert-DesktopDenied $privateFixture
    $privateFile = Join-Path $privateFixture 'runtime-write.bin'
    if ([GesherIdentityProbe]::CreateProbe($privateFile, $false) -ne 5) {
        throw 'An ordinary desktop token could create a file in the private-account fixture.'
    }
    $runtimeWrite = [GesherIdentityProbe]::CreateProbe($privateFile, $true)
    if ($runtimeWrite -ne 0 -or [IO.File]::ReadAllText($privateFile) -ne 'GSH') {
        throw "The real private SCM token could not write its NTFS fixture: $runtimeWrite"
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
            foreach ($guest in $guestProcesses) {
                if ([GesherIdentityProbe]::ProcessUser([uint32]$guest.ProcessId) -ne $runtimeSid) {
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
