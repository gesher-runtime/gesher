# Test-only LocalSystem probe for the optional installer integration test.
# Its reviewed source and request are copied into a protected Program Files
# directory by the parent test. The task returns data, never tokens or handles.
param([Parameter(Mandatory = $true)][string]$Request)

$ErrorActionPreference = 'Stop'
$resultDirectory = Split-Path -Parent ([IO.Path]::GetFullPath($Request))
$resultPath = Join-Path $resultDirectory 'result.json'
$result = @{ Success = $false; Error = 'The identity probe did not complete.' }
$exitCode = 1
$holder = $null

try {
    if ([Security.Principal.WindowsIdentity]::GetCurrent().User.Value -ne 'S-1-5-18') {
        throw 'The private-token probe requires the production authority caller, LocalSystem.'
    }
    $requestData = Get-Content -LiteralPath $Request -Raw | ConvertFrom-Json
    if ($requestData.Operation -notin 'WriteRuntimeFixture', 'QueryGuestUsers') {
        throw 'Unknown identity-probe operation.'
    }
    $serviceName = 'GesherProtectedRuntime'
    $installation = Join-Path $env:ProgramFiles 'Gesher Raw USB Broker'
    $holderImage = Join-Path $installation 'gesher-usb-authority.exe'
    $guestImage = Join-Path $installation 'runtime\dalvikvm.exe'
    $expectedSid = ([Security.Principal.NTAccount]::new('NT SERVICE', $serviceName)).Translate(
        [Security.Principal.SecurityIdentifier]).Value

    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;

public sealed class GesherSystemIdentityProbe : IDisposable {
    [StructLayout(LayoutKind.Sequential)] struct FileTime { public uint Low; public uint High; }
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, uint processId);
    [DllImport("kernel32.dll", SetLastError=true)] static extern uint GetProcessId(IntPtr process);
    [DllImport("kernel32.dll", SetLastError=true)] static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetProcessTimes(IntPtr process, out FileTime created, out FileTime exited, out FileTime kernel, out FileTime user);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool QueryFullProcessImageName(IntPtr process, uint flags, StringBuilder image, ref uint size);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool GetTokenInformation(IntPtr token, int kind, IntPtr value, uint size, out uint returned);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool ImpersonateLoggedOnUser(IntPtr token);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool RevertToSelf();
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr CreateFile(string path, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool WriteFile(IntPtr file, byte[] bytes, uint count, out uint written, IntPtr overlapped);

    IntPtr process, token;
    readonly bool canImpersonate;
    ulong creationTime;
    public uint ProcessId { get; private set; }
    public string CreationTime { get { return creationTime.ToString(); } }
    public string Image { get; private set; }
    public string UserSid { get; private set; }

    static void Require(bool value, string operation) {
        if (!value) {
            int error = Marshal.GetLastWin32Error();
            throw new Win32Exception(error, operation + " failed with Win32 " + error);
        }
    }
    ulong ReadCreationTime() {
        FileTime created, exited, kernel, user;
        Require(GetProcessTimes(process, out created, out exited, out kernel, out user),
            "GetProcessTimes(retained process)");
        return ((ulong)created.High << 32) | created.Low;
    }
    IntPtr QueryToken(int kind) {
        uint size;
        bool unexpected = GetTokenInformation(token, kind, IntPtr.Zero, 0, out size);
        int error = Marshal.GetLastWin32Error();
        if (unexpected || error != 122 || size == 0 || size > 4096) {
            throw new Win32Exception(error, "GetTokenInformation(" + kind + ", size) failed or returned invalid length " + size);
        }
        IntPtr buffer = Marshal.AllocHGlobal((int)size);
        try {
            Require(GetTokenInformation(token, kind, buffer, size, out size), "GetTokenInformation(" + kind + ")");
            return buffer;
        } catch { Marshal.FreeHGlobal(buffer); throw; }
    }
    public GesherSystemIdentityProbe(uint processId, string expectedImage, bool impersonate) {
        ProcessId = processId;
        canImpersonate = impersonate;
        try {
            process = OpenProcess(0x101000, false, processId); // QUERY_LIMITED_INFORMATION | SYNCHRONIZE
            Require(process != IntPtr.Zero, "OpenProcess(" + processId + ", QUERY_LIMITED_INFORMATION|SYNCHRONIZE=0x101000)");
            creationTime = ReadCreationTime();
            var image = new StringBuilder(32768);
            uint size = (uint)image.Capacity;
            Require(QueryFullProcessImageName(process, 0, image, ref size), "QueryFullProcessImageName(retained process)");
            Image = image.ToString();
            if (!String.Equals(Image, expectedImage, StringComparison.OrdinalIgnoreCase)) {
                throw new InvalidOperationException("The retained process image is not the exact installed image: " + Image);
            }
            VerifyLive();
            uint access = impersonate ? 0xAu : 0x8u; // primary QUERY|DUPLICATE, or QUERY only
            Require(OpenProcessToken(process, access, out token),
                "OpenProcessToken(" + processId + ", " + (impersonate ? "TOKEN_QUERY|TOKEN_DUPLICATE=0xA" : "TOKEN_QUERY=0x8") + ")");
            IntPtr kind = QueryToken(8); // TokenType
            try {
                if (Marshal.ReadInt32(kind) != 1) throw new InvalidOperationException("Expected a primary process token.");
            } finally { Marshal.FreeHGlobal(kind); }
            IntPtr user = QueryToken(1); // TokenUser
            try { UserSid = new SecurityIdentifier(Marshal.ReadIntPtr(user)).Value; }
            finally { Marshal.FreeHGlobal(user); }
            VerifyLive();
        } catch { Dispose(); throw; }
    }
    public void VerifyLive() {
        uint state = WaitForSingleObject(process, 0);
        if (state == 0xFFFFFFFF) Require(false, "WaitForSingleObject(retained process)");
        if (state != 258 || GetProcessId(process) != ProcessId || ReadCreationTime() != creationTime) {
            throw new InvalidOperationException("The retained process exited or changed identity during the probe.");
        }
    }
    public int WriteFixture(string path) {
        if (!canImpersonate) throw new InvalidOperationException("This probe has no primary-token duplicate right.");
        VerifyLive();
        Require(ImpersonateLoggedOnUser(token), "ImpersonateLoggedOnUser(private SCM primary token)");
        try {
            IntPtr file = CreateFile(path, 0x40000000, 7, IntPtr.Zero, 1, 0, IntPtr.Zero); // GENERIC_WRITE, CREATE_NEW
            if (file == new IntPtr(-1)) return Marshal.GetLastWin32Error();
            try {
                uint written;
                if (!WriteFile(file, new byte[] { 71, 83, 72 }, 3, out written, IntPtr.Zero)) return Marshal.GetLastWin32Error();
                return written == 3 ? 0 : 29;
            } finally { CloseHandle(file); }
        } finally {
            if (!RevertToSelf()) Environment.FailFast("RevertToSelf(private SCM token) failed");
        }
    }
    public void Dispose() {
        if (token != IntPtr.Zero) CloseHandle(token);
        if (process != IntPtr.Zero) CloseHandle(process);
        token = process = IntPtr.Zero;
    }
}
'@

    function Read-Holder {
        $service = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
        if (-not $service -or $service.State -ne 'Running' -or
            [uint32]$service.ProcessId -ne [uint32]$requestData.HolderProcessId -or
            $service.PathName -ine ('"' + $holderImage + '" runtime-identity') -or
            $service.StartName -ine ('NT SERVICE\' + $serviceName)) {
            throw 'The SCM holder PID, running state, image or virtual account changed during the probe.'
        }
        $service
    }

    $service = Read-Holder
    $write = $requestData.Operation -eq 'WriteRuntimeFixture'
    $holder = [GesherSystemIdentityProbe]::new([uint32]$service.ProcessId, $holderImage, $write)
    if ($holder.UserSid -ne $expectedSid) { throw 'The SCM holder TokenUser is not its configured private virtual account.' }
    $result = @{
        Success = $true
        Nonce = [string]$requestData.Nonce
        RuntimeSid = $holder.UserSid
        HolderProcessId = $holder.ProcessId
        HolderCreationTime = $holder.CreationTime
        WriteError = 0
        Guests = @()
    }
    if ($write) {
        $fixturePath = [IO.Path]::GetFullPath([string]$requestData.FixturePath)
        $parent = Split-Path -Parent $fixturePath
        if ((Split-Path -Leaf $fixturePath) -ne 'runtime-write.bin' -or
            (Split-Path -Leaf $parent) -ne 'private-account' -or
            (Split-Path -Leaf (Split-Path -Parent $parent)) -notlike 'gesher-identity-*' -or
            (Split-Path -Parent (Split-Path -Parent $parent)) -ine $env:ProgramFiles -or
            (Test-Path -LiteralPath $fixturePath)) {
            throw 'The positive-write probe requires a new file in its isolated Program Files test fixture.'
        }
        $result.WriteError = $holder.WriteFixture($fixturePath)
        if ($result.WriteError -ne 0) {
            throw "CreateFile(GENERIC_WRITE|CREATE_NEW)/WriteFile as private runtime failed with Win32 $($result.WriteError)."
        }
    } else {
        $ids = @($requestData.GuestProcessIds)
        if ($ids.Count -lt 2 -or $ids.Count -gt 64 -or @($ids | Select-Object -Unique).Count -ne $ids.Count) {
            throw 'The protected guest probe requires a distinct bounded process set including server and app.'
        }
        $guests = @()
        foreach ($processId in $ids) {
            $guest = [GesherSystemIdentityProbe]::new([uint32]$processId, $guestImage, $false)
            try {
                if ($guest.UserSid -ne $expectedSid) { throw 'A protected Android guest has the wrong TokenUser.' }
                $guests += @{
                    ProcessId = $guest.ProcessId
                    CreationTime = $guest.CreationTime
                    UserSid = $guest.UserSid
                    Image = $guest.Image
                }
                $guest.VerifyLive()
            } finally { $guest.Dispose() }
        }
        $result.Guests = $guests
    }
    $holder.VerifyLive()
    $null = Read-Holder
    $exitCode = 0
} catch {
    $result = @{ Success = $false; Nonce = [string]$requestData.Nonce; Error = $_.Exception.ToString() }
} finally {
    if ($holder) { $holder.Dispose() }
    $temporary = Join-Path $resultDirectory 'result.tmp'
    $result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $temporary -Encoding UTF8
    Move-Item -LiteralPath $temporary -Destination $resultPath -Force
}
exit $exitCode
