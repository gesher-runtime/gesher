param(
    [Parameter(Mandatory = $true)]
    [string]$Setup,
    [Parameter(Mandatory = $true)]
    [string]$BrokerImage
)

$ErrorActionPreference = 'Stop'
$service = 'GesherUsbAuthority'
$folder = Join-Path $env:ProgramFiles 'Gesher Raw USB Broker'
$exe = Join-Path $folder 'gesher-usb-authority.exe'
$uninstall = Join-Path $folder 'unins000.exe'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'The disposable installer test requires administrative rights.'
}
if ((Get-Service -Name $service -ErrorAction SilentlyContinue) -or
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

function Read-ServiceAcl {
    $sddl = @(& sc.exe sdshow $service | Where-Object { $_ -match '^[OGDS]:' }) -join ''
    if ($LASTEXITCODE -ne 0 -or -not $sddl) { throw 'Could not read the optional service ACL.' }
    [Security.AccessControl.RawSecurityDescriptor]::new($sddl)
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
    Assert-ProtectedOwner $folder
    Assert-ProtectedOwner $exe
    Assert-ProtectedOwner $uninstall
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
    # Exercise the uninstall stop path with the service still running.
} finally {
    if (Test-Path -LiteralPath $uninstall -PathType Leaf) {
        Invoke-Setup $uninstall
    }
}
$deadline = [DateTime]::UtcNow.AddSeconds(20)
while ($existing = Get-Service -Name $service -ErrorAction SilentlyContinue) {
    $existing.Dispose()
    if ([DateTime]::UtcNow -ge $deadline) { throw 'Uninstall left the optional service registered.' }
    Start-Sleep -Milliseconds 100
}
if (Test-Path -LiteralPath $exe) {
    throw 'Uninstall left the optional service or broker image behind.'
}
Write-Output 'PASS: optional USB authority installation, manual start, stop and uninstall'
