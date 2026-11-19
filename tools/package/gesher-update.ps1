# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Dot-source safe: definitions only. The control panel runs network work in a
# separate runspace; reading preferences or a version never loads the transport.

function ConvertTo-GesherStableVersion([object] $Value, [switch] $AllowTagPrefix) {
    if ($Value -isnot [string]) { return $null }
    $pattern = if ($AllowTagPrefix) { '\Av?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\z' } else { '\A(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\z' }
    $match = [regex]::Match($Value, $pattern, [Text.RegularExpressions.RegexOptions]::CultureInvariant)
    if (-not $match.Success) { return $null }
    $parts = @()
    foreach ($group in $match.Groups | Select-Object -Skip 1) {
        $part = 0
        if (-not [int]::TryParse($group.Value, [ref] $part) -or $part -gt 65535) { return $null }
        $parts += $part.ToString([Globalization.CultureInfo]::InvariantCulture)
    }
    return $parts -join '.'
}

# Installed and installer versions: a release (X.Y.Z) or a CI build after the
# latest release (X.Y.Z.N). [version] orders X.Y.Z below every X.Y.Z.N and
# every X.Y.Z.N below the next release. Release tags remain strictly X.Y.Z.
function ConvertTo-GesherBuildVersion([object] $Value) {
    if ($Value -isnot [string]) { return $null }
    $match = [regex]::Match($Value, '\A([^.]+\.[^.]+\.[^.]+)(?:\.([1-9][0-9]*))?\z',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)
    if (-not $match.Success) { return $null }
    $release = ConvertTo-GesherStableVersion $match.Groups[1].Value
    if (-not $release) { return $null }
    if (-not $match.Groups[2].Success) { return $release }
    $build = 0
    if (-not [int]::TryParse($match.Groups[2].Value, [ref] $build) -or $build -gt 65535) { return $null }
    return $release + '.' + $build.ToString([Globalization.CultureInfo]::InvariantCulture)
}

function Get-GesherInstalledVersion([string] $InstallDirectory) {
    try {
        $path = Join-Path $InstallDirectory 'gesher-version.ini'
        $file = Get-Item -LiteralPath $path -Force -ErrorAction Stop
        if ($file.PSIsContainer -or $file.Length -gt 4096) { return $null }
        $section = ''
        $found = $null
        $sections = 0
        foreach ($line in [IO.File]::ReadAllLines($file.FullName)) {
            $text = $line.Trim()
            if (-not $text -or $text.StartsWith(';') -or $text.StartsWith('#')) { continue }
            if ($text -match '^\[([^\]]+)\]$') {
                $section = $Matches[1]
                if ($section -ceq 'Gesher') { $sections++ }
                continue
            }
            if ($section -ceq 'Gesher' -and $text -match '^Version\s*=(.*)$') {
                if ($null -ne $found) { return $null }
                $found = $Matches[1].Trim()
            }
        }
        if ($sections -ne 1) { return $null }
        return ConvertTo-GesherBuildVersion $found
    } catch { return $null }
}

function Assert-GesherUpdateRelease([object] $Release) {
    $version = ConvertTo-GesherStableVersion $Release.Version
    $tagVersion = ConvertTo-GesherStableVersion $Release.Tag -AllowTagPrefix
    if (-not $version -or $version -cne $tagVersion) { throw 'Invalid Gesher release version.' }
    $assetName = "gesher-setup-$version.exe"
    $releaseUrl = "https://github.com/gesher-runtime/gesher/releases/tag/$($Release.Tag)"
    $downloadUrl = "https://github.com/gesher-runtime/gesher/releases/download/$($Release.Tag)/$assetName"
    if ($Release.AssetName -isnot [string] -or $Release.Url -isnot [string] -or $Release.DownloadUrl -isnot [string] -or
        $Release.AssetName -cne $assetName -or $Release.Url -cne $releaseUrl -or $Release.DownloadUrl -cne $downloadUrl) {
        throw 'The release does not contain the expected official Gesher installer.'
    }
    if (($Release.Size -isnot [int] -and $Release.Size -isnot [long]) -or $Release.Size -le 0 -or $Release.Size -gt 2147483648) {
        throw 'Invalid Gesher installer size.'
    }
    if ($Release.Sha256 -isnot [string] -or $Release.Sha256 -cnotmatch '\A[0-9a-fA-F]{64}\z') {
        throw 'The Gesher installer has no valid SHA-256 digest.'
    }
    return [pscustomobject]@{
        Version = $version; Tag = $Release.Tag; Url = $releaseUrl
        AssetName = $assetName; DownloadUrl = $downloadUrl
        Size = [long] $Release.Size; Sha256 = $Release.Sha256.ToLowerInvariant()
    }
}

function ConvertTo-GesherUpdateRelease([object] $Metadata) {
    if ($null -eq $Metadata -or $Metadata -is [array] -or
        $Metadata.draft -isnot [bool] -or $Metadata.prerelease -isnot [bool] -or
        $Metadata.draft -or $Metadata.prerelease) {
        throw 'The update response is not a published stable release.'
    }
    $version = ConvertTo-GesherStableVersion $Metadata.tag_name -AllowTagPrefix
    if (-not $version) { throw 'The release tag is not a supported stable version.' }
    $assetName = "gesher-setup-$version.exe"
    if ($Metadata.assets -isnot [array]) { throw 'The release assets are malformed.' }
    $assets = @($Metadata.assets | Where-Object { $_.name -ceq $assetName })
    if ($assets.Count -ne 1) { throw 'The release must contain exactly one matching Gesher installer.' }
    $asset = $assets[0]
    if ($asset.state -isnot [string] -or $asset.state -cne 'uploaded' -or $asset.digest -isnot [string] -or
        $asset.digest -cnotmatch '\Asha256:[0-9a-fA-F]{64}\z') {
        throw 'The published Gesher installer is incomplete or has no SHA-256 digest.'
    }
    return Assert-GesherUpdateRelease ([pscustomobject]@{
        Version = $version; Tag = $Metadata.tag_name; Url = $Metadata.html_url
        AssetName = $asset.name; DownloadUrl = $asset.browser_download_url
        Size = $asset.size; Sha256 = $asset.digest.Substring(7)
    })
}

function Initialize-GesherUpdateTransport {
    if ($PSVersionTable.PSEdition -eq 'Desktop') {
        # Windows PowerShell can target old Framework TLS defaults even when a
        # newer Framework is installed. Upgrade only a legacy-only process
        # selection; retain OS defaults (0) and explicit modern selections.
        # Setting 0 here is not sufficient on old-target Framework hosts whose
        # cached compatibility switch disallows system-default TLS versions.
        $protocol = [Net.ServicePointManager]::SecurityProtocol
        $legacy = [int][Net.SecurityProtocolType]::Ssl3 -bor
                  [int][Net.SecurityProtocolType]::Tls -bor [int][Net.SecurityProtocolType]::Tls11
        if ([int]$protocol -ne 0 -and ([int]$protocol -band (-bnot $legacy)) -eq 0) {
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        }
    }
    if ('Gesher.Updates.Transport' -as [type]) { return }
    $compiler = @{ ErrorAction = 'Stop' }
    if ($PSVersionTable.PSEdition -eq 'Desktop') { $compiler.ReferencedAssemblies = @('System.Net.Http') }
    Add-Type @compiler -TypeDefinition @'
using System;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Threading;

namespace Gesher.Updates {
    public static class Transport {
        const string Latest = "https://api.github.com/repos/gesher-runtime/gesher/releases/latest";
        const int JsonLimit = 1024 * 1024;

        static HttpClient CreateClient() {
            var handler = new HttpClientHandler();
            handler.AllowAutoRedirect = false;
            handler.UseProxy = true;
            handler.Proxy = WebRequest.DefaultWebProxy;
            // Certificate validation and proxy behavior remain the Windows defaults.
            var client = new HttpClient(handler);
            client.Timeout = Timeout.InfiniteTimeSpan;
            client.DefaultRequestHeaders.UserAgent.ParseAdd("Gesher-Updater/1.0");
            return client;
        }

        static void ValidateUri(Uri uri, bool metadata) {
            if (!uri.IsAbsoluteUri || uri.Scheme != "https" || !uri.IsDefaultPort ||
                uri.UserInfo.Length != 0 || uri.Fragment.Length != 0)
                throw new InvalidDataException("An update URL is not an approved HTTPS endpoint.");
            if (metadata) {
                if (uri.AbsoluteUri != Latest)
                    throw new InvalidDataException("The release API redirected outside the official repository.");
                return;
            }
            string host = uri.DnsSafeHost;
            if (host == "github.com") {
                if (!uri.AbsolutePath.StartsWith("/gesher-runtime/gesher/releases/download/", StringComparison.Ordinal) || uri.Query.Length != 0)
                    throw new InvalidDataException("The installer URL is not in the official repository.");
            } else if (host != "release-assets.githubusercontent.com" &&
                       host != "objects.githubusercontent.com" &&
                       host != "github-releases.githubusercontent.com") {
                throw new InvalidDataException("The installer redirected to an unapproved download host.");
            }
        }

        static HttpResponseMessage Send(HttpClient client, Uri uri, bool metadata, CancellationToken token) {
            for (int hop = 0; hop < 6; hop++) {
                ValidateUri(uri, metadata);
                HttpResponseMessage response;
                using (var request = new HttpRequestMessage(HttpMethod.Get, uri)) {
                    if (metadata) {
                        request.Headers.TryAddWithoutValidation("Accept", "application/vnd.github+json");
                        request.Headers.TryAddWithoutValidation("X-GitHub-Api-Version", "2022-11-28");
                    }
                    response = client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, token).GetAwaiter().GetResult();
                }
                int code = (int)response.StatusCode;
                if (code != 301 && code != 302 && code != 303 && code != 307 && code != 308) return response;
                using (response) {
                    if (response.Headers.Location == null)
                        throw new InvalidDataException("The update response has an empty redirect.");
                    uri = new Uri(uri, response.Headers.Location);
                }
            }
            throw new InvalidDataException("The update response has too many redirects.");
        }

        public static string ReadRelease(CancellationToken cancellation) {
            using (var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation))
            using (var client = CreateClient()) {
                deadline.CancelAfter(TimeSpan.FromSeconds(30));
                using (var response = Send(client, new Uri(Latest), true, deadline.Token))
                using (deadline.Token.Register(() => response.Dispose())) {
                    if (response.StatusCode == HttpStatusCode.NotFound) return null;
                    response.EnsureSuccessStatusCode();
                    if (response.Content.Headers.ContentLength > JsonLimit)
                        throw new InvalidDataException("The release response is too large.");
                    using (var input = response.Content.ReadAsStreamAsync().GetAwaiter().GetResult())
                    using (var output = new MemoryStream()) {
                        byte[] buffer = new byte[16384];
                        int count;
                        while ((count = input.ReadAsync(buffer, 0, buffer.Length, deadline.Token).GetAwaiter().GetResult()) != 0) {
                            if (output.Length + count > JsonLimit)
                                throw new InvalidDataException("The release response is too large.");
                            output.Write(buffer, 0, count);
                        }
                        deadline.Token.ThrowIfCancellationRequested();
                        return new UTF8Encoding(false, true).GetString(output.ToArray());
                    }
                }
            }
        }

        public static void Download(string address, string path, long expectedSize, CancellationToken cancellation) {
            if (expectedSize <= 0 || expectedSize > 2147483648L)
                throw new InvalidDataException("Invalid installer size.");
            using (var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation))
            using (var client = CreateClient()) {
                deadline.CancelAfter(TimeSpan.FromMinutes(10));
                using (var response = Send(client, new Uri(address), false, deadline.Token))
                using (deadline.Token.Register(() => response.Dispose())) {
                    response.EnsureSuccessStatusCode();
                    if (response.Content.Headers.ContentLength.HasValue && response.Content.Headers.ContentLength.Value != expectedSize)
                        throw new InvalidDataException("The installer length differs from the release metadata.");
                    using (var input = response.Content.ReadAsStreamAsync().GetAwaiter().GetResult())
                    using (var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None)) {
                        byte[] buffer = new byte[65536];
                        long received = 0;
                        int count;
                        while ((count = input.ReadAsync(buffer, 0, buffer.Length, deadline.Token).GetAwaiter().GetResult()) != 0) {
                            received += count;
                            if (received > expectedSize)
                                throw new InvalidDataException("The installer is larger than its release metadata.");
                            output.Write(buffer, 0, count);
                        }
                        deadline.Token.ThrowIfCancellationRequested();
                        if (received != expectedSize)
                            throw new InvalidDataException("The installer download is incomplete.");
                        output.Flush(true);
                    }
                }
            }
        }
    }
}
'@
}

function Invoke-GesherUpdateMetadataRequest([Threading.CancellationToken] $CancellationToken = [Threading.CancellationToken]::None) {
    Initialize-GesherUpdateTransport
    return [Gesher.Updates.Transport]::ReadRelease($CancellationToken)
}

function Get-GesherUpdate([string] $InstallDirectory, [Threading.CancellationToken] $CancellationToken = [Threading.CancellationToken]::None) {
    $CancellationToken.ThrowIfCancellationRequested()
    $current = Get-GesherInstalledVersion -InstallDirectory $InstallDirectory
    if (-not $current) { return [pscustomobject]@{ Status = 'UnknownVersion'; CurrentVersion = $null; Release = $null } }
    $json = Invoke-GesherUpdateMetadataRequest -CancellationToken $CancellationToken
    $CancellationToken.ThrowIfCancellationRequested()
    if ($null -eq $json) { return [pscustomobject]@{ Status = 'NoRelease'; CurrentVersion = $current; Release = $null } }
    $release = ConvertTo-GesherUpdateRelease -Metadata (ConvertFrom-Json -InputObject $json -ErrorAction Stop)
    $status = if ([version] $release.Version -gt [version] $current) { 'Available' } else { 'Current' }
    return [pscustomobject]@{ Status = $status; CurrentVersion = $current; Release = $release }
}

function Invoke-GesherUpdateDownload([string] $Uri, [string] $Path, [long] $ExpectedSize,
    [Threading.CancellationToken] $CancellationToken = [Threading.CancellationToken]::None) {
    Initialize-GesherUpdateTransport
    [Gesher.Updates.Transport]::Download($Uri, $Path, $ExpectedSize, $CancellationToken)
}

function ConvertTo-GesherInstallerVersion([object] $Value) {
    if ($Value -isnot [string]) { return $null }
    # Inno Setup pads its fixed-size textual version resource with ASCII spaces.
    # Remove only that trailing padding; all release version rules still apply.
    return ConvertTo-GesherBuildVersion -Value $Value.TrimEnd([char]0x20)
}

function Get-GesherUpdateInstallerVersion([string] $Path) {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw 'Gesher installers require Windows.' }
    return ConvertTo-GesherInstallerVersion -Value ([Diagnostics.FileVersionInfo]::GetVersionInfo($Path).ProductVersion)
}

function Assert-GesherUpdateInstaller([string] $Path, [object] $Release, [IO.Stream] $Stream,
    [Threading.CancellationToken] $CancellationToken = [Threading.CancellationToken]::None) {
    if ($Stream.Length -ne $Release.Size) { throw 'The installer length differs from the release metadata.' }
    $Stream.Position = 0
    $hash = [Security.Cryptography.SHA256]::Create()
    try {
        $buffer = New-Object byte[] 65536
        while (($count = $Stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
            $CancellationToken.ThrowIfCancellationRequested()
            $null = $hash.TransformBlock($buffer, 0, $count, $buffer, 0)
        }
        $CancellationToken.ThrowIfCancellationRequested()
        $null = $hash.TransformFinalBlock($buffer, 0, 0)
        $digest = [BitConverter]::ToString($hash.Hash).Replace('-', '').ToLowerInvariant()
    } finally { $hash.Dispose() }
    if ($digest -cne $Release.Sha256) { throw 'The installer SHA-256 digest does not match the release.' }
    $productVersion = Get-GesherUpdateInstallerVersion -Path $Path
    if ($productVersion -cne $Release.Version) { throw 'The installer product version does not match the release.' }
}

function Receive-GesherUpdate([object] $Release, [string] $DestinationDirectory,
    [Threading.CancellationToken] $CancellationToken = [Threading.CancellationToken]::None) {
    $releaseInfo = Assert-GesherUpdateRelease $Release
    $CancellationToken.ThrowIfCancellationRequested()
    $directory = Join-Path ([IO.Path]::GetFullPath($DestinationDirectory)) ('gesher-update-' + [guid]::NewGuid().ToString('N'))
    $partial = Join-Path $directory ($releaseInfo.AssetName + '.partial')
    $path = Join-Path $directory $releaseInfo.AssetName
    $completed = $false
    try {
        $null = [IO.Directory]::CreateDirectory($directory)
        Invoke-GesherUpdateDownload -Uri $releaseInfo.DownloadUrl -Path $partial -ExpectedSize $releaseInfo.Size -CancellationToken $CancellationToken
        $CancellationToken.ThrowIfCancellationRequested()
        $installerStream = [IO.File]::Open($partial, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        try { Assert-GesherUpdateInstaller -Path $partial -Release $releaseInfo -Stream $installerStream -CancellationToken $CancellationToken } finally { $installerStream.Dispose() }
        $CancellationToken.ThrowIfCancellationRequested()
        [IO.File]::Move($partial, $path)
        $completed = $true
        return [pscustomobject]@{ Path = $path; Directory = $directory; Release = $releaseInfo }
    } finally {
        if (-not $completed -and [IO.Directory]::Exists($directory)) { [IO.Directory]::Delete($directory, $true) }
    }
}

function Start-GesherUpdateProcess([string] $Path, [string] $Arguments, [string] $WorkingDirectory) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $Path
    $info.Arguments = $Arguments
    $info.WorkingDirectory = $WorkingDirectory
    $info.UseShellExecute = $false
    return [Diagnostics.Process]::Start($info)
}

function Start-GesherUpdateInstaller([object] $Download, [string] $InstallDirectory,
    [Threading.CancellationToken] $CancellationToken = [Threading.CancellationToken]::None) {
    $CancellationToken.ThrowIfCancellationRequested()
    $releaseInfo = Assert-GesherUpdateRelease $Download.Release
    $installPath = [IO.Path]::GetFullPath($InstallDirectory)
    $current = Get-GesherInstalledVersion -InstallDirectory $installPath
    if (-not $current -or [version] $releaseInfo.Version -le [version] $current) { throw 'The installer is not newer than this Gesher installation.' }
    $path = [IO.Path]::GetFullPath($Download.Path)
    $directory = [IO.Path]::GetFullPath($Download.Directory)
    $expectedPath = [IO.Path]::GetFullPath((Join-Path $directory $releaseInfo.AssetName))
    if (-not [string]::Equals($path, $expectedPath, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected downloaded installer path.' }
    if ($installPath.IndexOfAny([char[]] @([char] 0, [char] 10, [char] 13, [char] 34)) -ge 0) { throw 'Invalid installation directory.' }
    # Escape trailing backslashes before the closing quote using Windows argv rules.
    $quotedDirectory = '"' + [regex]::Replace($installPath, '(\\+)$', '$1$1') + '"'
    $installerStream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        # Keep the read handle open until CreateProcess returns: Windows cannot
        # replace or modify the verified bytes between validation and launch.
        Assert-GesherUpdateInstaller -Path $path -Release $releaseInfo -Stream $installerStream -CancellationToken $CancellationToken
        $CancellationToken.ThrowIfCancellationRequested()
        return Start-GesherUpdateProcess -Path $path -Arguments ('/NORESTART /DIR=' + $quotedDirectory) -WorkingDirectory $directory
    } finally { $installerStream.Dispose() }
}

function Get-GesherUpdatePreferences([string] $StateDirectory) {
    $defaults = [pscustomobject]@{ AutomaticChecks = $true; LastCheckUtc = $null }
    try {
        $path = Join-Path $StateDirectory 'update-settings.json'
        $file = Get-Item -LiteralPath $path -Force -ErrorAction Stop
        if ($file.PSIsContainer -or $file.Length -gt 16384) { return $defaults }
        $saved = [IO.File]::ReadAllText($file.FullName) | ConvertFrom-Json -ErrorAction Stop
        if ($saved -is [array] -or $saved.AutomaticChecks -isnot [bool]) { return $defaults }
        $last = if ($saved.LastCheckUtc -is [string]) { $saved.LastCheckUtc }
            elseif ($saved.LastCheckUtc -is [DateTime]) { $saved.LastCheckUtc.ToUniversalTime().ToString('o') }
            elseif ($saved.LastCheckUtc -is [DateTimeOffset]) { $saved.LastCheckUtc.UtcDateTime.ToString('o') }
            else { $null }
        return [pscustomobject]@{ AutomaticChecks = $saved.AutomaticChecks; LastCheckUtc = $last }
    } catch { return $defaults }
}

function Save-GesherUpdatePreferences([string] $StateDirectory, [object] $Preferences) {
    if ($Preferences.AutomaticChecks -isnot [bool] -or
        ($null -ne $Preferences.LastCheckUtc -and $Preferences.LastCheckUtc -isnot [string])) { throw 'Invalid update preferences.' }
    $directory = [IO.Path]::GetFullPath($StateDirectory)
    $null = [IO.Directory]::CreateDirectory($directory)
    $path = Join-Path $directory 'update-settings.json'
    $temporary = Join-Path $directory ('update-settings-' + [guid]::NewGuid().ToString('N') + '.tmp')
    $json = [pscustomobject]@{ AutomaticChecks = $Preferences.AutomaticChecks; LastCheckUtc = $Preferences.LastCheckUtc } | ConvertTo-Json -Compress
    try {
        [IO.File]::WriteAllText($temporary, $json, (New-Object Text.UTF8Encoding $false))
        if ([IO.File]::Exists($path)) { [IO.File]::Replace($temporary, $path, [NullString]::Value) }
        else { [IO.File]::Move($temporary, $path) }
    } finally { if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) } }
}

function Test-GesherUpdateCheckDue([object] $Preferences, [DateTimeOffset] $Now = [DateTimeOffset]::UtcNow) {
    if ($Preferences.AutomaticChecks -isnot [bool] -or -not $Preferences.AutomaticChecks) { return $false }
    $last = [DateTimeOffset]::MinValue
    if ($Preferences.LastCheckUtc -isnot [string] -or -not [DateTimeOffset]::TryParse($Preferences.LastCheckUtc,
        [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::AssumeUniversal, [ref] $last)) { return $true }
    if ($last -gt $Now) { return $true }
    return ($Now - $last) -ge [TimeSpan]::FromHours(24)
}
