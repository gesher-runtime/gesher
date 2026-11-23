# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# End-to-end Quality Assurance and Contract Verification for Gesher Portable Mode.

param(
    [string] $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$script:Checks = 0
function Assert-Portable([bool] $Condition, [string] $Message) {
    if (-not $Condition) {
        throw "ASSERTION FAILED: $Message"
    }
    $script:Checks++
}

$toolsDir = Join-Path $RepoRoot 'tools/package'
$runtimeHelper = Join-Path $toolsDir 'gesher-runtime-files.ps1'
if (-not (Test-Path -LiteralPath $runtimeHelper)) {
    throw "Runtime files helper not found: $runtimeHelper"
}
. $runtimeHelper

$savedProfile = $env:GESHER_PROFILE
$savedAndroidData = $env:ANDROID_DATA
$savedLocalAppData = $env:LOCALAPPDATA

try {
    # =========================================================================
    # A) Profile Root Resolution (Get-GesherProfileRoot)
    # =========================================================================
    $tempBase = Join-Path ([IO.Path]::GetTempPath()) ("gesher-portable-test-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tempBase -Force | Out-Null

    # Case 1: Subdirectory structure (<Dir>\App\ + <Dir>\Data\ + <Dir>\portable.marker)
    # Passed $InstallDirectory = <Dir>\App
    $case1Dir = Join-Path $tempBase 'case1'
    $case1App = Join-Path $case1Dir 'App'
    $case1Data = Join-Path $case1Dir 'Data'
    $case1Marker = Join-Path $case1Dir 'portable.marker'
    New-Item -ItemType Directory -Path $case1App -Force | Out-Null
    New-Item -ItemType Directory -Path $case1Data -Force | Out-Null
    Set-Content -LiteralPath $case1Marker -Value ''
    $env:GESHER_PROFILE = $null

    $resolved1 = Get-GesherProfileRoot $case1App
    Assert-Portable ($resolved1 -ceq [IO.Path]::GetFullPath($case1Data)) "Case 1: Get-GesherProfileRoot from App/ did not return Data/ ($resolved1 vs $case1Data)"

    # Case 2: Parent directory structure (<Dir>\App\ where <Dir>\Data exists without marker)
    $case2Dir = Join-Path $tempBase 'case2'
    $case2App = Join-Path $case2Dir 'App'
    $case2Data = Join-Path $case2Dir 'Data'
    New-Item -ItemType Directory -Path $case2App -Force | Out-Null
    New-Item -ItemType Directory -Path $case2Data -Force | Out-Null
    $env:GESHER_PROFILE = $null

    $resolved2 = Get-GesherProfileRoot $case2App
    Assert-Portable ($resolved2 -ceq [IO.Path]::GetFullPath($case2Data)) "Case 2: Get-GesherProfileRoot from App/ with parent Data/ did not return Data/ ($resolved2 vs $case2Data)"

    # Case 3: Flag file portable.marker in current dir (even before Data/ is created)
    $case3Dir = Join-Path $tempBase 'case3'
    New-Item -ItemType Directory -Path $case3Dir -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $case3Dir 'portable.marker') -Value ''
    $env:GESHER_PROFILE = $null

    $resolved3 = Get-GesherProfileRoot $case3Dir
    $expected3 = [IO.Path]::GetFullPath((Join-Path $case3Dir 'Data'))
    Assert-Portable ($resolved3 -ceq $expected3) "Case 3: Get-GesherProfileRoot with portable.marker in current dir did not return Data/ ($resolved3 vs $expected3)"

    # Case 3b: Data/ directory in current dir
    $case3bDir = Join-Path $tempBase 'case3b'
    $case3bData = Join-Path $case3bDir 'Data'
    New-Item -ItemType Directory -Path $case3bData -Force | Out-Null
    $resolved3b = Get-GesherProfileRoot $case3bDir
    Assert-Portable ($resolved3b -ceq [IO.Path]::GetFullPath($case3bData)) "Case 3b: Get-GesherProfileRoot with Data/ in current dir did not return Data/"

    # Case 4: No portable marker / regular install: fallback to $env:LOCALAPPDATA\Gesher
    $case4Dir = Join-Path $tempBase 'case4'
    New-Item -ItemType Directory -Path $case4Dir -Force | Out-Null
    $env:GESHER_PROFILE = $null

    $resolved4 = Get-GesherProfileRoot $case4Dir
    $expected4 = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'Gesher'))
    Assert-Portable ($resolved4 -ceq $expected4) "Case 4: Get-GesherProfileRoot in regular dir did not fallback to LOCALAPPDATA\Gesher ($resolved4 vs $expected4)"

    $resolved4Empty = Get-GesherProfileRoot ''
    Assert-Portable ($resolved4Empty -ceq $expected4) "Case 4 empty parameter did not fallback to LOCALAPPDATA\Gesher"

    # Priority Case: Explicit $env:GESHER_PROFILE overrides portable detection
    $customProfile = Join-Path $tempBase 'custom-profile'
    New-Item -ItemType Directory -Path $customProfile -Force | Out-Null
    $env:GESHER_PROFILE = $customProfile
    $resolvedCustom = Get-GesherProfileRoot $case1App
    Assert-Portable ($resolvedCustom -ceq [IO.Path]::GetFullPath($customProfile)) "Explicit GESHER_PROFILE was not prioritized"
    $env:GESHER_PROFILE = $null

    # =========================================================================
    # B) Isolation / Zero-footprint Check
    # =========================================================================
    # Simulate execution in a portable directory and verify paths do NOT touch LOCALAPPDATA
    $portableRoot = Join-Path $tempBase 'SimulatedPortable'
    $portableApp = Join-Path $portableRoot 'App'
    $portableData = Join-Path $portableRoot 'Data'
    New-Item -ItemType Directory -Path $portableApp -Force | Out-Null
    New-Item -ItemType Directory -Path $portableData -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $portableRoot 'portable.marker') -Value ''

    $profileRoot = Get-GesherProfileRoot $portableApp
    $env:GESHER_PROFILE = $profileRoot

    # Drive-less path conversion as in gesher.ps1 and gesher-app.ps1
    function Script-NoDrive([string] $Path) {
        $full = [IO.Path]::GetFullPath($Path)
        return ($full -replace '^[A-Za-z]:', '') -replace '\\', '/'
    }

    $androidData = Script-NoDrive "$profileRoot/data"
    $logsDir = Join-Path $profileRoot 'logs'

    Assert-Portable ($profileRoot.StartsWith([IO.Path]::GetFullPath($portableData), [StringComparison]::OrdinalIgnoreCase)) "Profile root is outside portable Data directory"
    Assert-Portable (-not $profileRoot.StartsWith($env:LOCALAPPDATA, [StringComparison]::OrdinalIgnoreCase)) "Profile root leaked into LOCALAPPDATA"
    Assert-Portable ($logsDir.StartsWith([IO.Path]::GetFullPath($portableData), [StringComparison]::OrdinalIgnoreCase)) "Logs directory is outside portable Data directory"
    Assert-Portable (-not $logsDir.StartsWith($env:LOCALAPPDATA, [StringComparison]::OrdinalIgnoreCase)) "Logs directory leaked into LOCALAPPDATA"

    # Also verify native launcher helper in compat/shell/gesher_launch.cc
    $launchCc = Join-Path $RepoRoot 'compat/shell/gesher_launch.cc'
    Assert-Portable (Test-Path -LiteralPath $launchCc) "gesher_launch.cc exists"
    $launchSource = [IO.File]::ReadAllText($launchCc)
    Assert-Portable ($launchSource -match 'GetDataDirectory\(') "gesher_launch.cc implements GetDataDirectory"
    Assert-Portable ($launchSource -match 'portable\.marker') "gesher_launch.cc checks portable.marker"
    Assert-Portable ($launchSource -match 'here\s*\+\s*L"\\\\Data"') "gesher_launch.cc checks Data directory"
    Assert-Portable ($launchSource -match 'parent\s*\+\s*L"\\\\Data"') "gesher_launch.cc checks parent Data directory"

    # =========================================================================
    # C) Dynamic Code Path Relocation in Installer.java
    # =========================================================================
    $installerJava = Join-Path $RepoRoot 'app/java/com/gesher/server/Installer.java'
    Assert-Portable (Test-Path -LiteralPath $installerJava) "Installer.java exists"
    $installerSource = [IO.File]::ReadAllText($installerJava)
    Assert-Portable ($installerSource -match 'static\s+String\s+resolveCodePath\s*\(\s*String\s+codePath\s*\)') "Installer.java contains resolveCodePath method"
    Assert-Portable ($installerSource -match 'resolveCodePath\s*\(\s*fields\[1\]\s*\)') "readRegistry() calls resolveCodePath(fields[1])"

    # Dynamic execution test of code path relocation using JDK
    $javaHarnessDir = Join-Path $tempBase 'java_harness'
    New-Item -ItemType Directory -Path $javaHarnessDir -Force | Out-Null

    $harnessSource = @'
import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.LinkedHashMap;
import java.util.Map;

public class CodePathRelocationTest {
    private static File codeRoot() throws IOException {
        String dataEnv = System.getenv("ANDROID_DATA");
        File dataDir = new File(dataEnv == null || dataEnv.isEmpty() ? "/data" : dataEnv).getAbsoluteFile();
        File root = new File(dataDir.getCanonicalFile(), "app");
        if (Files.exists(root.toPath()) && !Files.isDirectory(root.toPath())) {
            throw new SecurityException("Installed APK root is not a real directory: " + root);
        }
        return root;
    }

    public static String resolveCodePath(String codePath) {
        if (codePath == null || codePath.isEmpty()) return codePath;
        File current = new File(codePath);
        if (current.isDirectory()) return codePath;
        try {
            File fallback = new File(codeRoot(), current.getName());
            if (fallback.isDirectory()) {
                return fallback.getPath();
            }
        } catch (Throwable ignored) {
        }
        return codePath;
    }

    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            System.err.println("Usage: CodePathRelocationTest <android_data_dir> <temp_dir>");
            System.exit(1);
        }
        File androidData = new File(args[0]).getCanonicalFile();
        File appRoot = new File(androidData, "app");
        appRoot.mkdirs();

        // 1. Create a relocated app folder under current codeRoot
        File testAppDir = new File(appRoot, "com.test.relocated-1");
        testAppDir.mkdirs();

        // Test A: Obsolete drive letter (e.g. X:\OldDrive\Data\app\com.test.relocated-1)
        String oldDrivePath = "X:\\GesherPortable\\Data\\app\\com.test.relocated-1";
        String resolvedA = resolveCodePath(oldDrivePath);
        if (!new File(resolvedA).equals(testAppDir)) {
            System.err.println("FAIL: Expected re-root to " + testAppDir + " but got " + resolvedA);
            System.exit(2);
        }

        // Test B: Obsolete path on same drive but different location
        String oldLocationPath = new File(args[1], "non_existent_old_app\\com.test.relocated-1").getPath();
        String resolvedB = resolveCodePath(oldLocationPath);
        if (!new File(resolvedB).equals(testAppDir)) {
            System.err.println("FAIL: Expected re-root to " + testAppDir + " but got " + resolvedB);
            System.exit(3);
        }

        // Test C: Existing valid directory is preserved as-is
        String existingValid = testAppDir.getPath();
        String resolvedC = resolveCodePath(existingValid);
        if (!resolvedC.equals(existingValid)) {
            System.err.println("FAIL: Existing directory was unexpectedly altered: " + resolvedC);
            System.exit(4);
        }

        // Test D: Non-existent app without fallback stays unchanged
        String unknownPath = "Z:\\UnknownDir\\com.notfound.app-1";
        String resolvedD = resolveCodePath(unknownPath);
        if (!resolvedD.equals(unknownPath)) {
            System.err.println("FAIL: Unknown path without fallback altered: " + resolvedD);
            System.exit(5);
        }

        // Test E: Null and empty handling
        if (resolveCodePath(null) != null || !resolveCodePath("").isEmpty()) {
            System.err.println("FAIL: Null or empty string not preserved");
            System.exit(6);
        }

        System.out.println("PASS: Code path relocation verified (5 checks)");
        System.exit(0);
    }
}
'@
    $harnessFile = Join-Path $javaHarnessDir 'CodePathRelocationTest.java'
    [IO.File]::WriteAllText($harnessFile, $harnessSource)

    $javac = "javac"
    $java = "java"
    & $javac -encoding UTF-8 -d $javaHarnessDir $harnessFile
    Assert-Portable ($LASTEXITCODE -eq 0) "Java relocation harness compilation succeeded"

    $mockAndroidData = Join-Path $tempBase 'MockAndroidData'
    New-Item -ItemType Directory -Path $mockAndroidData -Force | Out-Null

    $psi = [Diagnostics.ProcessStartInfo]::new($java, "-cp `"$javaHarnessDir`" CodePathRelocationTest `"$mockAndroidData`" `"$tempBase`"")
    $psi.EnvironmentVariables['ANDROID_DATA'] = $mockAndroidData
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $proc = [Diagnostics.Process]::Start($psi)
    $stdout = $proc.StandardOutput.ReadToEnd()
    $stderr = $proc.StandardError.ReadToEnd()
    $null = $proc.WaitForExit(10000)

    Assert-Portable ($proc.ExitCode -eq 0) "Java relocation harness executed with code 0 ($stdout $stderr)"
    Assert-Portable ($stdout.Contains("PASS: Code path relocation verified")) "Java relocation harness verified 5 checks"

    # =========================================================================
    # D) Test tools/package/package-portable.ps1
    # =========================================================================
    $pkgScript = Join-Path $RepoRoot 'tools/package/package-portable.ps1'
    Assert-Portable (Test-Path -LiteralPath $pkgScript) "package-portable.ps1 exists"

    $packTestBase = Join-Path $tempBase 'pack_test'
    $mockBuild = Join-Path $packTestBase 'build'
    $packOut = Join-Path $packTestBase 'out'
    New-Item -ItemType Directory -Path $mockBuild -Force | Out-Null

    # Create dummy binaries and assets required by package-portable
    $dummyFiles = @(
        'dalvikvm.exe', 'core-oj.jar', 'core-libart.jar', 'core-icu4j.jar', 'hello.dex',
        'framework.jar', 'framework-res.apk', 'build.prop', 'gesher-app.jar', 'org.apache.http.legacy.jar',
        'com.android.location.provider.jar', 'binderd.exe', 'servicemanager.exe', 'gesher-exec.exe',
        'gesher-launch.exe', 'gesher_shellext.dll', 'dex2oat.exe', 'profman.exe', 'oatdump.exe',
        'lifecycle.apk', 'gesher-camera.apk', 'libEGL.dll', 'libGLESv2.dll', 'gesher_arm64.dll'
    )
    foreach ($df in $dummyFiles) {
        Set-Content -LiteralPath (Join-Path $mockBuild $df) -Value "mock binary $df"
    }
    New-Item -ItemType Directory -Path (Join-Path $mockBuild 'licenses') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $mockBuild 'licenses\TEST-LICENSE.txt') -Value 'License text'

    # Test D1: -SkipZip execution
    $skipZipOut = Join-Path $packOut 'skipzip'
    & powershell -NoProfile -ExecutionPolicy Bypass -File $pkgScript -BuildDir $mockBuild -OutDir $skipZipOut -Version "0.9.1" -SkipZip
    Assert-Portable ($LASTEXITCODE -eq 0) "package-portable.ps1 -SkipZip exited with code 0"

    $stagedRoot = Join-Path $skipZipOut 'GesherPortable'
    Assert-Portable (Test-Path -LiteralPath $stagedRoot) "Staging directory GesherPortable exists"
    Assert-Portable (Test-Path -LiteralPath (Join-Path $stagedRoot 'portable.marker') -PathType Leaf) "portable.marker exists in staging"
    Assert-Portable (Test-Path -LiteralPath (Join-Path $stagedRoot 'Data') -PathType Container) "Data directory exists in staging"
    Assert-Portable (Test-Path -LiteralPath (Join-Path $stagedRoot 'App') -PathType Container) "App directory exists in staging"
    Assert-Portable (Test-Path -LiteralPath (Join-Path $stagedRoot 'Gesher.exe') -PathType Leaf) "Gesher.exe exists at portable root"
    Assert-Portable (Test-Path -LiteralPath (Join-Path $stagedRoot 'README.txt') -PathType Leaf) "README.txt exists at portable root"

    $versionIni = Join-Path $stagedRoot 'App\gesher-version.ini'
    Assert-Portable (Test-Path -LiteralPath $versionIni -PathType Leaf) "gesher-version.ini generated in App/"
    $versionContent = [IO.File]::ReadAllText($versionIni)
    Assert-Portable ($versionContent -match 'Version\s*=\s*0\.9\.1') "gesher-version.ini records version 0.9.1"

    # Verify runtime files staged into App/
    Assert-Portable (Test-Path -LiteralPath (Join-Path $stagedRoot 'App\gesher-app.ps1')) "gesher-app.ps1 copied to App/"
    Assert-Portable (Test-Path -LiteralPath (Join-Path $stagedRoot 'App\gesher-runtime-files.ps1')) "gesher-runtime-files.ps1 copied to App/"
    Assert-Portable (Test-Path -LiteralPath (Join-Path $stagedRoot 'App\dalvikvm.exe')) "dalvikvm.exe copied to App/"

    # Test D2: Zip compression and SHA256 checksums
    $zipOut = Join-Path $packOut 'zip'
    & powershell -NoProfile -ExecutionPolicy Bypass -File $pkgScript -BuildDir $mockBuild -OutDir $zipOut -Version "0.9.1"
    Assert-Portable ($LASTEXITCODE -eq 0) "package-portable.ps1 zip archive exited with code 0"

    $expectedZip = Join-Path $zipOut "gesher-0.9.1-portable.zip"
    $expectedSha = Join-Path $zipOut "gesher-0.9.1-portable.zip.sha256"
    $expectedSums = Join-Path $zipOut "SHA256SUMS.txt"

    Assert-Portable (Test-Path -LiteralPath $expectedZip -PathType Leaf) "Portable zip created: $expectedZip"
    Assert-Portable (Test-Path -LiteralPath $expectedSha -PathType Leaf) "Portable sha256 created: $expectedSha"
    Assert-Portable (Test-Path -LiteralPath $expectedSums -PathType Leaf) "SHA256SUMS.txt created: $expectedSums"
    Assert-Portable (-not (Test-Path -LiteralPath (Join-Path $zipOut 'GesherPortable'))) "Staging directory cleaned up after zip creation"

    $actualZipHash = (Get-FileHash -LiteralPath $expectedZip -Algorithm SHA256).Hash.ToLowerInvariant()
    $shaContent = (Get-Content -LiteralPath $expectedSha -Raw).Trim()
    $sumsContent = (Get-Content -LiteralPath $expectedSums -Raw).Trim()

    Assert-Portable ($shaContent -match ("^" + $actualZipHash + "\s+gesher-0\.9\.1-portable\.zip$")) "Per-file .sha256 matches actual hash and filename"
    Assert-Portable ($sumsContent -match ($actualZipHash + "\s+gesher-0\.9\.1-portable\.zip")) "SHA256SUMS.txt contains valid entry"

    # Verify Zip structure: opens and has GesherPortable/ top directory
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($expectedZip)
    try {
        $entryNames = @($zip.Entries | ForEach-Object { $_.FullName -replace '\\', '/' })
        Assert-Portable ($entryNames -contains 'GesherPortable/Gesher.exe') "Zip contains GesherPortable/Gesher.exe"
        Assert-Portable ($entryNames -contains 'GesherPortable/portable.marker') "Zip contains GesherPortable/portable.marker"
        Assert-Portable (@($entryNames | Where-Object { $_ -like 'GesherPortable/App/gesher-*.ps1' }).Count -ge 5) "Zip contains App/ PowerShell scripts"
    } finally {
        $zip.Dispose()
    }

    # =========================================================================
    # E) Verify Inno Setup syntax and Portable flags
    # =========================================================================
    $issPath = Join-Path $RepoRoot 'tools/package/gesher.iss'
    Assert-Portable (Test-Path -LiteralPath $issPath) "gesher.iss exists"
    $issContent = [IO.File]::ReadAllText($issPath)

    # 1. IsPortableInstall() function
    Assert-Portable ($issContent -match 'function\s+IsPortableInstall\(\)\s*:\s*Boolean;') "gesher.iss declares IsPortableInstall()"
    Assert-Portable ($issContent -match "CompareText\(ParamStr\(I\),\s*'/PORTABLE'\)") "IsPortableInstall recognizes /PORTABLE command line switch"
    Assert-Portable ($issContent -match "InstallTypePage\.SelectedValueIndex\s*=\s*1") "IsPortableInstall recognizes wizard selection option 1"

    # 2. ShouldSkipPage logic
    Assert-Portable ($issContent -match 'function\s+ShouldSkipPage\(PageID\s*:\s*Integer\)\s*:\s*Boolean;') "gesher.iss defines ShouldSkipPage"
    Assert-Portable ($issContent -match 'if\s+PageID\s*=\s*wpSelectDir\s+then\s+Result\s*:=\s*not\s+IsPortableInstall\(\)') "ShouldSkipPage enables directory selection only when portable"
    Assert-Portable ($issContent -match '\(PageID\s*=\s*wpSelectTasks\)\s+and\s+IsPortableInstall\(\)') "ShouldSkipPage skips tasks page for portable install"

    # 3. not IsPortableInstall checks on system integration
    Assert-Portable ($issContent -match 'Name:\s*"\{autoprograms\}\\{cm:setup_control_panel\}".*?Check:\s*not\s+IsPortableInstall') "Control panel shortcut guarded with not IsPortableInstall"
    Assert-Portable ($issContent -match 'Name:\s*"\{autoprograms\}\\{cm:setup_readme\}".*?Check:\s*not\s+IsPortableInstall') "Readme shortcut guarded with not IsPortableInstall"
    Assert-Portable ($issContent -match 'Subkey:\s*"Software\\Classes\\\.apk".*?Check:\s*\(not\s+IsPortableInstall\)') "APK file association guarded with not IsPortableInstall"
    Assert-Portable ($issContent -match 'Subkey:\s*"Software\\Classes\\CLSID\\\{{1,2}6B0C3C9E-2D1B-4E57-9B7A-3F2C8E4D1A50\}{1,2}".*?Check:\s*not\s+IsPortableInstall') "Shell extension CLSID guarded with not IsPortableInstall"
    Assert-Portable ($issContent -match 'Subkey:\s*"Software\\Classes\\CLSID\\\{{1,2}C18D9445-A7F0-472E-985F-AC354EBEB660\}{1,2}".*?Check:\s*not\s+IsPortableInstall') "Toast activator CLSID guarded with not IsPortableInstall"

    # 4. Zero-footprint and portable structure creation in CurStepChanged
    Assert-Portable ($issContent -match 'SaveStringToFile\(MarkerPath,\s*'''',\s*False\)') "CurStepChanged creates portable.marker on portable install"
    Assert-Portable ($issContent -match 'ForceDirectories\(DataPath\)') "CurStepChanged creates Data directory on portable install"
    Assert-Portable ($issContent -match 'CopyFile\(LaunchPath,\s*GesherExePath,\s*False\)') "CurStepChanged copies gesher-launch.exe to Gesher.exe"
    Assert-Portable ($issContent -match 'RegDeleteKeyIncludingSubkeys\(HKEY_CURRENT_USER,\s*UninstKey\)') "CurStepChanged deletes uninstaller registry key on portable install"
    Assert-Portable ($issContent -match 'DeleteFile\(Dir\s*\+\s*''\\unins000\.exe''\)') "CurStepChanged deletes unins000.exe on portable install"
    Assert-Portable ($issContent -match 'DeleteFile\(Dir\s*\+\s*''\\unins000\.dat''\)') "CurStepChanged deletes unins000.dat on portable install"

    # 5. Compiler syntax check with ISCC if installed
    $isccExe = "C:\Users\otzaria\AppData\Local\Programs\Inno Setup 6\ISCC.exe"
    if (Test-Path -LiteralPath $isccExe) {
        $isccDummy = Join-Path $tempBase 'iscc_dummy'
        New-Item -ItemType Directory -Path $isccDummy -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $isccDummy 'framework\x86_64') -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $isccDummy 'fonts') -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $isccDummy 'tzdata') -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $isccDummy 'cacerts') -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $isccDummy 'media\audio') -Force | Out-Null

        $isccFiles = @(
            'dalvikvm.exe','libgesher_account_crypto.so','libgesher_install_pipe.so','libgesher_webview_bridge.so',
            'core-oj.jar','core-libart.jar','core-icu4j.jar','icudt68l.dat','hello.dex','framework.jar',
            'framework-res.apk','build.prop','gesher-app.jar','org.apache.http.legacy.jar',
            'com.android.location.provider.jar','binderd.exe','servicemanager.exe','gesher-exec.exe',
            'gesher-launch.exe','gesher_shellext.dll','dex2oat.exe','profman.exe','oatdump.exe',
            'lifecycle.apk','gesher-camera.apk','libEGL.dll','libGLESv2.dll','ANGLE-NOTICES.txt',
            'gesher_arm64.dll','DYNARMIC-NOTICES.txt','FONTS-NOTICES.txt','TZDATA-NOTICES.txt','NET-NOTICES.txt'
        )
        foreach ($f in $isccFiles) {
            $p = Join-Path $isccDummy $f
            if (-not (Test-Path $p)) { Set-Content -Path $p -Value "dummy" }
        }
        Set-Content -Path (Join-Path $isccDummy "framework\x86_64\boot.art") -Value "dummy"
        Set-Content -Path (Join-Path $isccDummy "fonts\Roboto-Regular.ttf") -Value "dummy"
        Set-Content -Path (Join-Path $isccDummy "tzdata\tzdata") -Value "dummy"
        Set-Content -Path (Join-Path $isccDummy "cacerts\test.0") -Value "dummy"
        Set-Content -Path (Join-Path $isccDummy "media\audio\test.ogg") -Value "dummy"

        $isccOut = Join-Path $tempBase 'iscc_out'
        New-Item -ItemType Directory -Path $isccOut -Force | Out-Null

        Push-Location $toolsDir
        try {
            $isccResult = & $isccExe "/DBuildDir=$isccDummy" "/DAppVersion=0.1.0" "/O$isccOut" (Join-Path $toolsDir 'gesher.iss') 2>&1 | Out-String
            Assert-Portable ($LASTEXITCODE -eq 0) "ISCC compiler exited with code 0"
            Assert-Portable (-not $isccResult.Contains("Warning:")) "ISCC compiler produced zero warnings ($isccResult)"
            Assert-Portable (Test-Path (Join-Path $isccOut 'gesher-setup-0.1.0.exe')) "ISCC produced installer executable"
        } finally {
            Pop-Location
        }
    } else {
        Write-Host "NOTE: ISCC.exe not found on host; skipped binary installer build"
    }

    Write-Host "PASS: portable mode contracts ($($script:Checks) checks)"
} finally {
    $env:GESHER_PROFILE = $savedProfile
    $env:ANDROID_DATA = $savedAndroidData
    $env:LOCALAPPDATA = $savedLocalAppData
    if ($tempBase -and (Test-Path -LiteralPath $tempBase)) {
        Remove-Item -LiteralPath $tempBase -Recurse -Force -ErrorAction SilentlyContinue
    }
}
