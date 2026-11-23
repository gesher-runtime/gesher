<#
.SYNOPSIS
    Assembles and packages a portable Gesher distribution.

.DESCRIPTION
    Creates a self-contained portable package with layout:
    GesherPortable/
    ├── Gesher.exe              (copied from gesher-launch.exe, with gesher.ico embedded)
    ├── portable.marker         (empty flag file)
    ├── README.txt              (from tools/package/README.txt or README.he.txt)
    ├── Data/                   (empty directory ready for user profile, apps, logs)
    └── App/                    (all runtime files)

.PARAMETER BuildDir
    Directory containing built binaries and runtime assets. If omitted,
    searches dist, $env:LOCALAPPDATA\Programs\Gesher, and out.

.PARAMETER OutDir
    Output directory for the portable archive and checksums (default: installer).

.PARAMETER Version
    Package version string. If omitted, queried via release_version.py or defaults to 0.1.0.

.PARAMETER SkipZip
    If set, leaves the staged GesherPortable directory without archiving.
#>
[CmdletBinding()]
param(
    [string] $BuildDir = '',
    [string] $OutDir = 'installer',
    [string] $Version = '',
    [switch] $SkipZip
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = [IO.Path]::GetFullPath((Join-Path $ScriptDir '..\..'))
$PackageDir = [IO.Path]::GetFullPath((Join-Path $RepoRoot 'tools\package'))

# 1. Resolve BuildDir
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $candidates = @(
        (Join-Path $RepoRoot 'dist'),
        "$env:LOCALAPPDATA\Programs\Gesher",
        (Join-Path $RepoRoot 'out')
    )
    foreach ($cand in $candidates) {
        if ((Test-Path -LiteralPath $cand) -and (
            (Test-Path -LiteralPath (Join-Path $cand 'dalvikvm.exe')) -or
            (Test-Path -LiteralPath (Join-Path $cand 'gesher-launch.exe'))
        )) {
            $BuildDir = $cand
            break
        }
    }
    if ([string]::IsNullOrWhiteSpace($BuildDir)) {
        foreach ($cand in $candidates) {
            if (Test-Path -LiteralPath $cand) {
                $BuildDir = $cand
                break
            }
        }
    }
    if ([string]::IsNullOrWhiteSpace($BuildDir)) {
        throw "Could not determine BuildDir. Please specify -BuildDir."
    }
}
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
Write-Host "Using BuildDir: $BuildDir"

# 2. Resolve Version
if ([string]::IsNullOrWhiteSpace($Version)) {
    try {
        $vScript = Join-Path $RepoRoot 'tools\ci\release_version.py'
        if (Test-Path -LiteralPath $vScript) {
            $vOutput = & python $vScript 2>$null
            if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($vOutput)) {
                $Version = $vOutput.Trim()
            }
        }
    } catch { }

    if ([string]::IsNullOrWhiteSpace($Version)) {
        $ini = Join-Path $BuildDir 'gesher-version.ini'
        if (Test-Path -LiteralPath $ini) {
            $text = Get-Content -LiteralPath $ini -Raw -ErrorAction SilentlyContinue
            if ($text -match 'Version\s*=\s*([0-9\.]+)') {
                $Version = $Matches[1].Trim()
            }
        }
    }

    if ([string]::IsNullOrWhiteSpace($Version)) {
        $Version = '0.1.0'
    }
}
$Version = $Version.Trim()
Write-Host "Packaging version: $Version"

# 3. Resolve OutDir
if (-not [IO.Path]::IsPathRooted($OutDir)) {
    $OutDir = Join-Path $RepoRoot $OutDir
}
$OutDir = [IO.Path]::GetFullPath($OutDir)
if (-not (Test-Path -LiteralPath $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}

# 4. Create staging directory structure
$StagingDir = Join-Path $OutDir 'GesherPortable'
if (Test-Path -LiteralPath $StagingDir) {
    Remove-Item -LiteralPath $StagingDir -Recurse -Force
}
New-Item -ItemType Directory -Path $StagingDir -Force | Out-Null

$AppDir = Join-Path $StagingDir 'App'
New-Item -ItemType Directory -Path $AppDir -Force | Out-Null

$DataDir = Join-Path $StagingDir 'Data'
New-Item -ItemType Directory -Path $DataDir -Force | Out-Null

# 5. Root portable files
$MarkerPath = Join-Path $StagingDir 'portable.marker'
New-Item -ItemType File -Path $MarkerPath -Force | Out-Null

$readmeSrc = $null
if (Test-Path -LiteralPath (Join-Path $PackageDir 'README.txt')) {
    $readmeSrc = Join-Path $PackageDir 'README.txt'
} elseif (Test-Path -LiteralPath (Join-Path $BuildDir 'README.txt')) {
    $readmeSrc = Join-Path $BuildDir 'README.txt'
} elseif (Test-Path -LiteralPath (Join-Path $PackageDir 'README.he.txt')) {
    $readmeSrc = Join-Path $PackageDir 'README.he.txt'
}
if ($readmeSrc) {
    Copy-Item -LiteralPath $readmeSrc -Destination (Join-Path $StagingDir 'README.txt') -Force
}

# 6. Copy gesher-launch.exe to Gesher.exe and embed gesher.ico
$launchExe = $null
if (Test-Path -LiteralPath (Join-Path $BuildDir 'gesher-launch.exe')) {
    $launchExe = Join-Path $BuildDir 'gesher-launch.exe'
} elseif (Test-Path -LiteralPath (Join-Path $PackageDir 'gesher-launch.exe')) {
    $launchExe = Join-Path $PackageDir 'gesher-launch.exe'
}
if ($launchExe) {
    $targetGesherExe = Join-Path $StagingDir 'Gesher.exe'
    Copy-Item -LiteralPath $launchExe -Destination $targetGesherExe -Force

    $icoFile = $null
    if (Test-Path -LiteralPath (Join-Path $PackageDir 'gesher.ico')) {
        $icoFile = Join-Path $PackageDir 'gesher.ico'
    } elseif (Test-Path -LiteralPath (Join-Path $BuildDir 'gesher.ico')) {
        $icoFile = Join-Path $BuildDir 'gesher.ico'
    }

    if ($icoFile) {
        try {
            if (-not ([System.Management.Automation.PSTypeName]'IconEmbedder').Type) {
                $csharp = @"
using System;
using System.IO;
using System.Runtime.InteropServices;

public class IconEmbedder {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr BeginUpdateResource(string pFileName, bool bDeleteExistingResources);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool UpdateResource(IntPtr hUpdate, IntPtr lpType, IntPtr lpName, ushort wLanguage, byte[] lpData, uint cbData);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool EndUpdateResource(IntPtr hUpdate, bool fDiscard);

    public static bool EmbedIcon(string exePath, string icoPath) {
        try {
            byte[] icoBytes = File.ReadAllBytes(icoPath);
            if (icoBytes.Length < 6) return false;
            ushort type = BitConverter.ToUInt16(icoBytes, 2);
            ushort count = BitConverter.ToUInt16(icoBytes, 4);
            if (type != 1 || count == 0) return false;

            IntPtr hUpdate = BeginUpdateResource(exePath, false);
            if (hUpdate == IntPtr.Zero) return false;

            byte[] groupData = new byte[6 + count * 14];
            Buffer.BlockCopy(icoBytes, 0, groupData, 0, 6);

            for (int i = 0; i < count; i++) {
                int dirOffset = 6 + i * 16;
                int grpOffset = 6 + i * 14;

                Buffer.BlockCopy(icoBytes, dirOffset, groupData, grpOffset, 12);
                ushort nID = (ushort)(i + 1);
                byte[] idBytes = BitConverter.GetBytes(nID);
                groupData[grpOffset + 12] = idBytes[0];
                groupData[grpOffset + 13] = idBytes[1];

                uint bytesInRes = BitConverter.ToUInt32(icoBytes, dirOffset + 8);
                uint imageOffset = BitConverter.ToUInt32(icoBytes, dirOffset + 12);

                byte[] imageBytes = new byte[bytesInRes];
                Buffer.BlockCopy(icoBytes, (int)imageOffset, imageBytes, 0, (int)bytesInRes);

                if (!UpdateResource(hUpdate, (IntPtr)3, (IntPtr)nID, 0, imageBytes, bytesInRes)) {
                    EndUpdateResource(hUpdate, true);
                    return false;
                }
            }

            if (!UpdateResource(hUpdate, (IntPtr)14, (IntPtr)1, 0, groupData, (uint)groupData.Length)) {
                EndUpdateResource(hUpdate, true);
                return false;
            }

            return EndUpdateResource(hUpdate, false);
        } catch {
            return false;
        }
    }
}
"@
                Add-Type -TypeDefinition $csharp -ErrorAction SilentlyContinue
            }
            [IconEmbedder]::EmbedIcon($targetGesherExe, $icoFile) | Out-Null
        } catch { }
    }
}

# 7. Helper to copy directories
function Copy-DirectoryIfExists {
    param([string] $Source, [string] $Destination)
    if (Test-Path -LiteralPath $Source) {
        if (-not (Test-Path -LiteralPath $Destination)) {
            New-Item -ItemType Directory -Path $Destination -Force | Out-Null
        }
        Copy-Item -LiteralPath "$Source\*" -Destination $Destination -Recurse -Force
    }
}

# 8. Stage runtime files into App/
$runtimeFiles = @(
    'dalvikvm.exe',
    'core-oj.jar',
    'core-libart.jar',
    'core-icu4j.jar',
    'hello.dex',
    'framework.jar',
    'framework-res.apk',
    'build.prop',
    'gesher-app.jar',
    'org.apache.http.legacy.jar',
    'com.android.location.provider.jar',
    'binderd.exe',
    'servicemanager.exe',
    'gesher-exec.exe',
    'gesher-launch.exe',
    'gesher_shellext.dll',
    'dex2oat.exe',
    'profman.exe',
    'oatdump.exe',
    'lifecycle.apk',
    'gesher-camera.apk',
    'libEGL.dll',
    'libGLESv2.dll',
    'gesher_arm64.dll'
)

foreach ($f in $runtimeFiles) {
    $src = $null
    if (Test-Path -LiteralPath (Join-Path $BuildDir $f)) {
        $src = Join-Path $BuildDir $f
    } elseif (Test-Path -LiteralPath (Join-Path $PackageDir $f)) {
        $src = Join-Path $PackageDir $f
    }
    if ($src) {
        Copy-Item -LiteralPath $src -Destination (Join-Path $AppDir $f) -Force
    }
}

# Wildcards from BuildDir
Get-ChildItem -LiteralPath $BuildDir -Filter 'libgesher_*.so' -File -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $AppDir -Force
}
Get-ChildItem -LiteralPath $BuildDir -Filter 'icudt*l.dat' -File -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $AppDir -Force
}
Get-ChildItem -LiteralPath $BuildDir -Filter '*.jar' -File -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $AppDir -Force
}

# Directories from BuildDir / PackageDir
Copy-DirectoryIfExists (Join-Path $BuildDir 'framework\x86_64') (Join-Path $AppDir 'framework\x86_64')
Copy-DirectoryIfExists (Join-Path $BuildDir 'fonts') (Join-Path $AppDir 'fonts')
Copy-DirectoryIfExists (Join-Path $BuildDir 'tzdata') (Join-Path $AppDir 'tzdata')
Copy-DirectoryIfExists (Join-Path $BuildDir 'cacerts') (Join-Path $AppDir 'cacerts')
Copy-DirectoryIfExists (Join-Path $BuildDir 'media\audio') (Join-Path $AppDir 'media\audio')
Copy-DirectoryIfExists (Join-Path $PackageDir 'etc\permissions') (Join-Path $AppDir 'etc\permissions')
Copy-DirectoryIfExists (Join-Path $BuildDir 'etc\permissions') (Join-Path $AppDir 'etc\permissions')

# Licenses
$AppLicenses = Join-Path $AppDir 'licenses'
New-Item -ItemType Directory -Path $AppLicenses -Force | Out-Null
Copy-DirectoryIfExists (Join-Path $PackageDir 'licenses') $AppLicenses
Copy-DirectoryIfExists (Join-Path $BuildDir 'licenses') $AppLicenses
foreach ($notice in @('ANGLE-NOTICES.txt', 'DYNARMIC-NOTICES.txt', 'FONTS-NOTICES.txt', 'TZDATA-NOTICES.txt', 'NET-NOTICES.txt')) {
    $noticePath = Join-Path $BuildDir $notice
    if (Test-Path -LiteralPath $noticePath) {
        Copy-Item -LiteralPath $noticePath -Destination $AppLicenses -Force
    }
}

# Package scripts
$scripts = @(
    'gesher.cmd',
    'gesher.ps1',
    'gesher-app.cmd',
    'gesher-app.ps1',
    'gesher-app-microg.ps1',
    'gesher-control-panel.ps1',
    'gesher-update.ps1',
    'gesher-runtime-files.ps1',
    'gesher-ui.ps1',
    'ui-messages.json',
    'gesher.ico'
)
foreach ($s in $scripts) {
    $src = $null
    if (Test-Path -LiteralPath (Join-Path $PackageDir $s)) {
        $src = Join-Path $PackageDir $s
    } elseif (Test-Path -LiteralPath (Join-Path $BuildDir $s)) {
        $src = Join-Path $BuildDir $s
    }
    if ($src) {
        Copy-Item -LiteralPath $src -Destination (Join-Path $AppDir $s) -Force
    }
}

# Version file
$iniContent = "[Gesher]`r`nVersion=$Version`r`n"
Set-Content -LiteralPath (Join-Path $AppDir 'gesher-version.ini') -Value $iniContent -Encoding ASCII

# 9. Packaging into zip if -not $SkipZip
if (-not $SkipZip) {
    $zipName = "gesher-$Version-portable.zip"
    $zipPath = Join-Path $OutDir $zipName
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }

    Write-Host "Creating portable archive: $zipPath"
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory($StagingDir, $zipPath, [System.IO.Compression.CompressionLevel]::Optimal, $true)

    $hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()

    $shaFile = "$zipPath.sha256"
    "$hash  $zipName" | Set-Content -LiteralPath $shaFile -Encoding ASCII

    $sumsFile = Join-Path $OutDir 'SHA256SUMS.txt'
    $sumsLine = "$hash  $zipName"
    if (Test-Path -LiteralPath $sumsFile) {
        $lines = @(Get-Content -LiteralPath $sumsFile | Where-Object { $_ -notmatch "\s+$([regex]::Escape($zipName))$" })
        $lines += $sumsLine
        $lines | Set-Content -LiteralPath $sumsFile -Encoding ASCII
    } else {
        $sumsLine | Set-Content -LiteralPath $sumsFile -Encoding ASCII
    }

    Remove-Item -LiteralPath $StagingDir -Recurse -Force
    Write-Host "Portable package created: $zipPath (SHA256: $hash)"
} else {
    Write-Host "Portable package staged at: $StagingDir"
}
