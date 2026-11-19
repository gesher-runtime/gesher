param([string] $UpdaterPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-update.ps1'))

$ErrorActionPreference = 'Stop'
# Exercise actual production functions. Substitute only network, PE metadata and
# process creation boundaries; real installation is tested by the package CI job.
$tokens = $null; $parseErrors = $null
$null = [System.Management.Automation.Language.Parser]::ParseFile(
    [IO.Path]::GetFullPath($UpdaterPath), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
. $UpdaterPath
$installerVersionReader = ${function:Get-GesherUpdateInstallerVersion}
$script:Checks = 0
function Assert-Update([bool] $Condition, [string] $Message) {
    if (-not $Condition) { throw $Message }
    $script:Checks++
}
function Assert-UpdateThrows([scriptblock] $Action, [string] $Message) {
    $threw = $false
    try { & $Action | Out-Null } catch { $threw = $true }
    Assert-Update $threw $Message
}
function Assert-InvalidRelease($Metadata, [string] $Message) {
    $release = $null
    try { $release = ConvertTo-GesherUpdateRelease -Metadata $Metadata } catch { }
    Assert-Update ($null -eq $release) $Message
}
$script:Payload = [Text.Encoding]::UTF8.GetBytes('Gesher release installer fixture')
$sha = [Security.Cryptography.SHA256]::Create()
try { $script:PayloadHash = ([BitConverter]::ToString($sha.ComputeHash($script:Payload))).Replace('-', '').ToLowerInvariant() }
finally { $sha.Dispose() }
function New-ReleaseMetadata([string] $Version = '0.2.0') {
    [PSCustomObject]@{
        tag_name = "v$Version"
        html_url = "https://github.com/gesher-runtime/gesher/releases/tag/v$Version"
        draft = $false
        prerelease = $false
        assets = @([PSCustomObject]@{
            name = "gesher-setup-$Version.exe"
            browser_download_url = "https://github.com/gesher-runtime/gesher/releases/download/v$Version/gesher-setup-$Version.exe"
            size = $script:Payload.Length
            state = 'uploaded'
            digest = "sha256:$script:PayloadHash"
        })
    }
}
$script:MetadataCalls = 0
$script:MetadataMode = 'release'
$script:MetadataJson = New-ReleaseMetadata | ConvertTo-Json -Depth 8
function Invoke-GesherUpdateMetadataRequest {
    param([Threading.CancellationToken] $CancellationToken = [Threading.CancellationToken]::None)
    $script:MetadataCalls++
    $CancellationToken.ThrowIfCancellationRequested()
    switch ($script:MetadataMode) {
        'missing' { return $null }
        'forbidden' { throw [Net.WebException]::new('GitHub HTTP 403: rate limit') }
        'timeout' { throw [TimeoutException]::new('GitHub request timed out') }
        'invalid' { return '{invalid JSON' }
        default { return $script:MetadataJson }
    }
}
$script:DownloadCalls = 0
$script:DownloadMode = 'complete'
function Invoke-GesherUpdateDownload {
    param([uri] $Uri, [string] $Path, [long] $ExpectedSize,
        [Threading.CancellationToken] $CancellationToken = [Threading.CancellationToken]::None)
    $script:DownloadCalls++
    $script:DownloadUri = $Uri.AbsoluteUri
    $CancellationToken.ThrowIfCancellationRequested()
    switch ($script:DownloadMode) {
        'partial' {
            [IO.File]::WriteAllBytes($Path, $script:Payload[0..4])
            throw [IO.IOException]::new('Connection ended during download')
        }
        'cancel' {
            [IO.File]::WriteAllBytes($Path, $script:Payload[0..4])
            throw [OperationCanceledException]::new('Download cancelled')
        }
        'truncated' { [IO.File]::WriteAllBytes($Path, $script:Payload[0..4]); return }
        'corrupt' { [IO.File]::WriteAllBytes($Path, [byte[]]::new($script:Payload.Length)); return }
        default { [IO.File]::WriteAllBytes($Path, $script:Payload) }
    }
}
$script:InstallerVersion = '0.2.0'
function Get-GesherUpdateInstallerVersion([string] $Path) { $script:InstallerVersion }
$script:ProcessCalls = 0
$script:ProcessFailure = $false
function Start-GesherUpdateProcess {
    param([string] $Path, $Arguments, [string] $WorkingDirectory)
    $script:ProcessCalls++
    $script:StartedInstaller = $Path
    $script:StartedArguments = @($Arguments) -join ' '
    $script:StartedDirectory = $WorkingDirectory
    if ($script:ProcessFailure) { throw [ComponentModel.Win32Exception]::new('Installer could not start') }
    [Diagnostics.Process]::GetCurrentProcess()
}
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$work = Join-Path $tempRoot ('gesher update checks ' + [char]0x05E9 + ' ' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $work | Out-Null
try {
    Assert-Update ((ConvertTo-GesherInstallerVersion '0.2.0') -ceq '0.2.0') 'Canonical installer version was changed'
    Assert-Update ((ConvertTo-GesherInstallerVersion ('0.2.0' + (' ' * 43))) -ceq '0.2.0') 'Inno Setup ASCII version padding was not removed'
    Assert-Update ((ConvertTo-GesherInstallerVersion ('0.2.0.526' + (' ' * 39))) -ceq '0.2.0.526') 'CI build installer version was rejected'
    foreach ($invalid in @($null, '', 42, ' 0.2.0', "0.2.0`t", "0.2.0`r`n", '0 .2.0',
        '0.2.0.0   ', '0.2.0.01 ', '0.2.0.65536 ', '0.2.0.1.2 ', '01.2.0 ', 'v0.2.0 ', '0.2.0-beta ', '0.2.65536 ')) {
        Assert-Update ($null -eq (ConvertTo-GesherInstallerVersion $invalid)) "Invalid padded installer version accepted: $invalid"
    }
    if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
        # Exercise FileVersionInfo itself via the saved production reader.
        # A PE version resource with the same padding as the actual Inno artifact
        # prevents fixture-only normalization from hiding a Windows read failure.
        $versionFixture = Join-Path $work 'installer-version-fixture.dll'
        $paddedVersion = '0.2.0' + (' ' * 43)
        Add-Type -OutputAssembly $versionFixture -TypeDefinition @"
using System.Reflection;
[assembly: AssemblyVersion("0.2.0.0")]
[assembly: AssemblyFileVersion("0.2.0.0")]
[assembly: AssemblyInformationalVersion("$paddedVersion")]
public static class GesherInstallerVersionFixture {}
"@
        $rawFixtureVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($versionFixture).ProductVersion
        Assert-Update ($rawFixtureVersion -ceq $paddedVersion) 'Windows fixture did not retain the Inno-style ProductVersion padding'
        Assert-Update ((& $installerVersionReader -Path $versionFixture) -ceq '0.2.0') 'Actual Windows installer-version reader rejected padded PE metadata'
    }
    $install = Join-Path $work 'installed Gesher'
    $downloads = Join-Path $work 'downloads'
    New-Item -ItemType Directory -Path $install, $downloads | Out-Null
    $versionPath = Join-Path $install 'gesher-version.ini'
    Assert-Update ($null -eq (Get-GesherInstalledVersion -InstallDirectory $install)) 'Missing installation version was invented'
    $unknown = Get-GesherUpdate -InstallDirectory $install
    Assert-Update ($unknown.Status -eq 'UnknownVersion') 'Unversioned installation was offered an upgrade'
    Assert-Update ($script:MetadataCalls -eq 0) 'Unversioned installation made a redundant network request'
    foreach ($invalid in '0.1', '01.2.3', '0.1.2.0', '0.1.2.03', '0.1.2.65536', '0.1.2.3.4', '0.1.2.', '0.2.0-beta', '0.2.0+build', '1.2.65536', 'v0.2.0', 'garbage') {
        [IO.File]::WriteAllText($versionPath, "[Gesher]`r`nVersion=$invalid`r`n")
        Assert-Update ($null -eq (Get-GesherInstalledVersion -InstallDirectory $install)) "Invalid installed version accepted: $invalid"
    }
    [IO.File]::WriteAllText($versionPath, "[Other]`r`nVersion=0.1.9`r`n")
    Assert-Update ($null -eq (Get-GesherInstalledVersion -InstallDirectory $install)) 'An unrelated INI section supplied the installed version'
    foreach ($invalidIni in "[Gesher]`nVersion=0.1.9`nVersion=0.2.0`n", "[Gesher]`nVersion=0.1.9`n[Gesher]`n", ('#' * 4097)) {
        [IO.File]::WriteAllText($versionPath, $invalidIni)
        Assert-Update ($null -eq (Get-GesherInstalledVersion -InstallDirectory $install)) 'Ambiguous or oversized installed version metadata was accepted'
    }
    [IO.File]::WriteAllText($versionPath, "[Gesher]`r`nVersion=0.1.9`r`n")
    Assert-Update ((Get-GesherInstalledVersion -InstallDirectory $install) -ceq '0.1.9') 'Installed version was not read from the Gesher section'
    [IO.File]::WriteAllText($versionPath, "[Gesher]`r`nVersion=0.2.0.526`r`n")
    Assert-Update ((Get-GesherInstalledVersion -InstallDirectory $install) -ceq '0.2.0.526') 'CI build installed version was not read'
    [IO.File]::WriteAllText($versionPath, "[Gesher]`r`nVersion=0.1.9`r`n")
    $metadata = New-ReleaseMetadata
    $release = ConvertTo-GesherUpdateRelease -Metadata $metadata
    Assert-Update ($release.Version -ceq '0.2.0' -and $release.Tag -ceq 'v0.2.0') 'Stable release identity was not preserved'
    Assert-Update ($release.Sha256 -ceq $script:PayloadHash -and $release.Size -eq $script:Payload.Length) 'Release integrity metadata was not retained'
    Assert-Update ($release.AssetName -ceq 'gesher-setup-0.2.0.exe' -and $release.DownloadUrl -ceq $metadata.assets[0].browser_download_url) 'Wrong installer asset was selected'
    foreach ($tag in 'v0.2.0-beta', 'v0.2.0+build', 'v0.2', 'v0.2.0.0', 'v00.2.0', 'v65536.0.0', '../0.2.0') {
        $bad = New-ReleaseMetadata; $bad.tag_name = $tag
        Assert-InvalidRelease $bad "Non-stable or malformed tag accepted: $tag"
    }
    foreach ($flag in 'draft', 'prerelease') {
        foreach ($value in $true, 'false', 0, $null) {
            $bad = New-ReleaseMetadata; $bad.$flag = $value
            Assert-InvalidRelease $bad "Invalid release flag accepted: $flag=$value"
        }
    }
    foreach ($url in 'http://github.com/gesher-runtime/gesher/releases/tag/v0.2.0',
        'https://github.com/another/repo/releases/tag/v0.2.0',
        'https://github.com/gesher-runtime/gesher/releases/tag/v0.2.0?other=1') {
        $bad = New-ReleaseMetadata; $bad.html_url = $url
        Assert-InvalidRelease $bad 'Unexpected release page URL accepted'
    }
    foreach ($url in 'http://github.com/gesher-runtime/gesher/releases/download/v0.2.0/gesher-setup-0.2.0.exe',
        'https://github.com/another/repo/releases/download/v0.2.0/gesher-setup-0.2.0.exe',
        'https://github.com/gesher-runtime/gesher/releases/download/v0.3.0/gesher-setup-0.2.0.exe',
        'https://github.com/gesher-runtime/gesher/releases/download/v0.2.0/gesher-setup-0.2.0.exe?other=1') {
        $bad = New-ReleaseMetadata; $bad.assets[0].browser_download_url = $url
        Assert-InvalidRelease $bad 'Unexpected asset download URL accepted'
    }
    foreach ($size in 0, -1, 1.5, '31', 2147483649, $null) {
        $bad = New-ReleaseMetadata; $bad.assets[0].size = $size
        Assert-InvalidRelease $bad "Invalid download length accepted: $size"
    }
    foreach ($digest in $null, '', 'sha256:abc', ('sha1:' + $script:PayloadHash), ('sha256:' + ('g' * 64))) {
        $bad = New-ReleaseMetadata; $bad.assets[0].digest = $digest
        Assert-InvalidRelease $bad 'Invalid or missing SHA-256 accepted'
    }
    $bad = New-ReleaseMetadata; $bad.assets = @()
    Assert-InvalidRelease $bad 'Release without an installer accepted'
    $bad = New-ReleaseMetadata; $bad.assets = @($bad.assets[0], $bad.assets[0])
    Assert-InvalidRelease $bad 'Ambiguous duplicate installer assets accepted'
    $bad = New-ReleaseMetadata; $bad.assets[0].name = 'gesher-setup-0.2.0-arm64.exe'
    Assert-InvalidRelease $bad 'An incompatible installer name was used as a fallback'
    # Actual numeric ordering, including the lexical 9-versus-10 trap and rollback refusal.
    foreach ($case in @(
        @{ Installed = '0.1.9'; Latest = '0.1.10'; Status = 'Available' },
        @{ Installed = '0.1.10'; Latest = '0.1.10'; Status = 'Current' },
        @{ Installed = '0.2.0'; Latest = '0.1.10'; Status = 'Current' },
        @{ Installed = '1.0.0'; Latest = '0.65535.65535'; Status = 'Current' },
        # A CI build after a release (X.Y.Z.N) is newer than X.Y.Z and older than the next release.
        @{ Installed = '0.2.0.526'; Latest = '0.2.0'; Status = 'Current' },
        @{ Installed = '0.2.0.526'; Latest = '0.2.1'; Status = 'Available' },
        @{ Installed = '0.1.9.65535'; Latest = '0.1.10'; Status = 'Available' },
        @{ Installed = '0.0.0.640'; Latest = '0.2.0'; Status = 'Available' })) {
        [IO.File]::WriteAllText($versionPath, "[Gesher]`nVersion=$($case.Installed)`n")
        $script:MetadataJson = New-ReleaseMetadata $case.Latest | ConvertTo-Json -Depth 8
        $result = Get-GesherUpdate -InstallDirectory $install
        Assert-Update ($result.Status -ceq $case.Status -and $result.CurrentVersion -ceq $case.Installed) "Version comparison failed: $($case.Installed) to $($case.Latest)"
    }
    [IO.File]::WriteAllText($versionPath, "[Gesher]`nVersion=0.1.9`n")
    $script:MetadataMode = 'missing'
    Assert-Update ((Get-GesherUpdate -InstallDirectory $install).Status -eq 'NoRelease') 'HTTP 404/no latest release was not a normal empty state'
    foreach ($mode in 'forbidden', 'timeout', 'invalid') {
        $script:MetadataMode = $mode
        Assert-UpdateThrows { Get-GesherUpdate -InstallDirectory $install } "Metadata failure reported a usable release: $mode"
    }
    $script:MetadataMode = 'release'
    $script:MetadataJson = New-ReleaseMetadata | ConvertTo-Json -Depth 8
    $cts = [Threading.CancellationTokenSource]::new()
    try {
        $cts.Cancel()
        Assert-UpdateThrows { Get-GesherUpdate -InstallDirectory $install -CancellationToken $cts.Token } 'Cancelled release check succeeded'
    } finally { $cts.Dispose() }
    # Keep unrelated files while cleaning only this attempt's incomplete download.
    $sentinel = Join-Path $downloads 'unrelated.txt'
    [IO.File]::WriteAllText($sentinel, 'keep')
    foreach ($mode in 'partial', 'cancel', 'truncated', 'corrupt') {
        $script:DownloadMode = $mode
        Assert-UpdateThrows { Receive-GesherUpdate -Release $release -DestinationDirectory $downloads } "Failed download was promoted: $mode"
        Assert-Update ([IO.File]::ReadAllText($sentinel) -ceq 'keep') 'Failed download cleanup changed an unrelated file'
        Assert-Update (@(Get-ChildItem -LiteralPath $downloads -Force).Count -eq 1) "Failed download left an executable or temporary directory: $mode"
    }
    $script:DownloadMode = 'complete'
    $download = Receive-GesherUpdate -Release $release -DestinationDirectory $downloads
    Assert-Update (Test-Path -LiteralPath $download.Path -PathType Leaf) 'Verified installer was not promoted to a file'
    Assert-Update ((Get-FileHash -LiteralPath $download.Path -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $script:PayloadHash) 'Promoted installer differs from verified bytes'
    Assert-Update ($script:DownloadUri -ceq $release.DownloadUrl) 'Download used a different asset from the checked release'
    $cts = [Threading.CancellationTokenSource]::new()
    try {
        $cts.Cancel()
        $callsBefore = $script:DownloadCalls
        Assert-UpdateThrows { Receive-GesherUpdate -Release $release -DestinationDirectory $downloads -CancellationToken $cts.Token } 'Pre-cancelled download succeeded'
        Assert-Update ($script:DownloadCalls -eq $callsBefore -and @(Get-ChildItem -LiteralPath $downloads -Force).Count -eq 2) 'Pre-cancelled download started work or left temporary state'
    } finally { $cts.Dispose() }

    $script:InstallerVersion = '0.2.0'
    $process = Start-GesherUpdateInstaller -Download $download -InstallDirectory $install
    try { Assert-Update ($null -ne $process -and $script:ProcessCalls -eq 1) 'Verified installer was not launched exactly once' }
    finally { if ($process) { $process.Dispose() } }
    Assert-Update ($script:StartedInstaller -ceq $download.Path) 'Installer handoff used a different file'
    Assert-Update ($script:StartedArguments -ceq ('/NORESTART /DIR="' + $install + '"')) 'Installer command did not preserve the quoted destination or unexpectedly requested unattended installation'
    Assert-Update ($script:StartedDirectory -ceq $download.Directory) 'Installer working directory differs from the verified download'
    $cts = [Threading.CancellationTokenSource]::new()
    try {
        $cts.Cancel()
        Assert-UpdateThrows { Start-GesherUpdateInstaller -Download $download -InstallDirectory $install -CancellationToken $cts.Token } 'Pre-cancelled installer handoff succeeded'
        Assert-Update ($script:ProcessCalls -eq 1) 'Cancelled installer was launched'
    } finally { $cts.Dispose() }
    foreach ($alreadyInstalled in '0.2.0', '0.3.0') {
        [IO.File]::WriteAllText($versionPath, "[Gesher]`nVersion=$alreadyInstalled`n")
        Assert-UpdateThrows { Start-GesherUpdateInstaller -Download $download -InstallDirectory $install } 'Installer ignored a changed installed version after downloading'
        Assert-Update ($script:ProcessCalls -eq 1) 'A now-redundant or older installer was launched'
    }
    [IO.File]::WriteAllText($versionPath, "[Gesher]`nVersion=0.1.9`n")
    $script:InstallerVersion = '0.3.0'
    Assert-UpdateThrows { Start-GesherUpdateInstaller -Download $download -InstallDirectory $install } 'Installer ProductVersion mismatch reached process launch'
    Assert-Update ($script:ProcessCalls -eq 1) 'Wrong-version installer was launched'
    $script:InstallerVersion = '0.2.0.526'
    Assert-UpdateThrows { Start-GesherUpdateInstaller -Download $download -InstallDirectory $install } 'A CI build installer was accepted as the release installer'
    Assert-Update ($script:ProcessCalls -eq 1) 'CI build installer was launched for a release'
    $script:InstallerVersion = '0.2.0'
    [IO.File]::WriteAllBytes($download.Path, [byte[]]::new($script:Payload.Length))
    Assert-UpdateThrows { Start-GesherUpdateInstaller -Download $download -InstallDirectory $install } 'Modified installer passed the final integrity check'
    Assert-Update ($script:ProcessCalls -eq 1) 'Modified installer was launched'
    [IO.File]::WriteAllBytes($download.Path, $script:Payload[0..4])
    Assert-UpdateThrows { Start-GesherUpdateInstaller -Download $download -InstallDirectory $install } 'Truncated installer passed the final integrity check'
    Assert-Update ($script:ProcessCalls -eq 1) 'Truncated installer was launched'
    [IO.File]::WriteAllBytes($download.Path, $script:Payload)
    $script:ProcessFailure = $true
    Assert-UpdateThrows { Start-GesherUpdateInstaller -Download $download -InstallDirectory $install } 'Process creation failure was reported as a successful handoff'
    Assert-Update (Test-Path -LiteralPath $download.Path) 'Launch failure removed the validated installer before a retry'
    $script:ProcessFailure = $false
    $state = Join-Path $work 'update state'
    $preferences = Get-GesherUpdatePreferences -StateDirectory $state
    Assert-Update ($preferences.AutomaticChecks -is [bool] -and $preferences.AutomaticChecks) 'New installation did not enable automatic checks'
    $now = [DateTime]::SpecifyKind([DateTime]'2026-11-14T12:00:00', [DateTimeKind]::Utc)
    Assert-Update (Test-GesherUpdateCheckDue -Preferences $preferences -Now $now) 'First automatic check was suppressed'
    $preferences.AutomaticChecks = $false
    $preferences.LastCheckUtc = $now.ToString('o')
    Save-GesherUpdatePreferences -StateDirectory $state -Preferences $preferences
    $loaded = Get-GesherUpdatePreferences -StateDirectory $state
    Assert-Update (-not $loaded.AutomaticChecks -and $loaded.LastCheckUtc -and [DateTimeOffset] $loaded.LastCheckUtc -eq [DateTimeOffset] $preferences.LastCheckUtc) 'Update preferences did not persist'
    Assert-Update (-not (Test-GesherUpdateCheckDue -Preferences $loaded -Now $now.AddDays(2))) 'Disabled automatic checks were scheduled'
    $loaded.AutomaticChecks = $true
    Assert-Update (-not (Test-GesherUpdateCheckDue -Preferences $loaded -Now $now.AddHours(23))) 'Automatic check ignored the daily interval'
    Assert-Update (Test-GesherUpdateCheckDue -Preferences $loaded -Now $now.AddHours(24)) 'Daily check was suppressed after the interval'
    Assert-Update (Test-GesherUpdateCheckDue -Preferences $loaded -Now $now.AddDays(-2)) 'A future timestamp suppressed checks after a clock correction'
    $loaded.LastCheckUtc = 'not a date'
    Assert-Update (Test-GesherUpdateCheckDue -Preferences $loaded -Now $now) 'Malformed timestamp permanently suppressed checks'
    $stateFile = Join-Path $state 'update-settings.json'
    [IO.File]::WriteAllText($stateFile, '{invalid JSON')
    $loaded = Get-GesherUpdatePreferences -StateDirectory $state
    Assert-Update ($loaded.AutomaticChecks -and (Test-GesherUpdateCheckDue -Preferences $loaded -Now $now)) 'Corrupt preferences did not recover to enabled checks'
    [IO.File]::WriteAllText($stateFile, '{"AutomaticChecks":"false","LastCheckUtc":null}')
    $loaded = Get-GesherUpdatePreferences -StateDirectory $state
    Assert-Update ($loaded.AutomaticChecks -is [bool] -and $loaded.AutomaticChecks) 'A string was accepted as a persisted boolean preference'
    Save-GesherUpdatePreferences -StateDirectory $state -Preferences ([PSCustomObject]@{ AutomaticChecks = $false; LastCheckUtc = $null })
    Assert-Update (-not (Get-GesherUpdatePreferences -StateDirectory $state).AutomaticChecks) 'Atomic replacement did not update existing settings'
    $saved = [IO.File]::ReadAllText($stateFile)
    Assert-UpdateThrows { Save-GesherUpdatePreferences -StateDirectory $state -Preferences ([PSCustomObject]@{ AutomaticChecks = 'false'; LastCheckUtc = $null }) } 'Invalid preference type was written'
    Assert-Update ([IO.File]::ReadAllText($stateFile) -ceq $saved) 'Rejected preference write changed the existing setting'
    Assert-Update (@(Get-ChildItem -LiteralPath $state -Force).Count -eq 1) 'Preference persistence left temporary files'
    "PASS: GitHub release updater contracts ($script:Checks checks)"
} finally {
    $parent = [IO.Path]::GetFullPath((Split-Path $work)).TrimEnd([IO.Path]::DirectorySeparatorChar)
    if ($parent -ne $tempRoot.TrimEnd([IO.Path]::DirectorySeparatorChar)) { throw 'Unexpected updater test cleanup path' }
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}
