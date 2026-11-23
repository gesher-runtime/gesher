$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $repoRoot 'tools/package/gesher-app-microg.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
$function = $ast.Find({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Initialize-MicrogDefaults' }, $true)
. ([scriptblock]::Create($function.Extent.Text))
$profile = Join-Path ([IO.Path]::GetTempPath()) ('gesher-defaults-test-' + [guid]::NewGuid())
try {
    Initialize-MicrogDefaults $profile
    # Check-in and Cloud Messaging remain microG's own opt-in settings.
    $prefsDir = Join-Path $profile 'data/data/com.google.android.gms/shared_prefs'
    if (Test-Path -LiteralPath $prefsDir) { throw 'microG preferences must not be written by setup' }

    $permFile = Join-Path $profile 'data/system/runtime-permissions'
    $lines = @(Get-Content $permFile)
    if ($lines.Count -ne 3) { throw 'Expected three default grants' }
    foreach ($line in $lines) {
        if ($line -notmatch '^com\.google\.android\.gms\\:[a-f0-9]{64}\\:android\.permission\.[A-Z_]+=granted$') { throw "Invalid Java Properties key: $line" }
    }
    # A second setup must retain user preferences and permission denials.
    New-Item -ItemType Directory -Force -Path $prefsDir | Out-Null
    $prefs = Join-Path $prefsDir 'com.google.android.gms_preferences.xml'
    $existing = '<map><boolean name="checkin_enable_service" value="false" /><string name="token">keep</string></map>'
    [IO.File]::WriteAllText($prefs, $existing, [Text.UTF8Encoding]::new($false))
    $lines[0] = $lines[0].Replace('=granted', '=denied')
    $lines += 'other.package\:signer\:permission=denied'
    Set-Content $permFile $lines -Encoding ASCII
    Initialize-MicrogDefaults $profile
    if ([IO.File]::ReadAllText($prefs, [Text.UTF8Encoding]::new($false)) -cne $existing) { throw 'Existing microG preferences changed' }
    if (@(Get-ChildItem -LiteralPath $prefsDir -Force).Count -ne 1) { throw 'Setup added microG preference files' }
    if ((@(Get-Content $permFile) -join "`n") -cne ($lines -join "`n")) { throw 'Existing permission decisions overwritten or duplicated' }
    'PASS: microG defaults use escaped property keys, leave microG settings untouched and preserve denials'
} finally {
    # This path is created exclusively for this test beneath the temp directory.
    $full = [IO.Path]::GetFullPath($profile)
    if ((Split-Path $full) -ne [IO.Path]::GetTempPath().TrimEnd('\')) { throw 'Unexpected test cleanup path' }
    if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
}
