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
    $permFile = Join-Path $profile 'data/system/runtime-permissions'
    $lines = @(Get-Content $permFile)
    if ($lines.Count -ne 3) { throw 'Expected three default grants' }
    foreach ($line in $lines) {
        if ($line -notmatch '^com\.google\.android\.gms\\:[a-f0-9]{64}\\:android\.permission\.[A-Z_]+=granted$') { throw "Invalid Java Properties key: $line" }
    }
    # A second setup must retain user settings, check-in tokens and denials.
    $prefs = Join-Path $profile 'data/data/com.google.android.gms/shared_prefs/checkin.xml'
    $existing = '<map><boolean name="checkin_enable_service" value="false" /><string name="token">keep</string></map>'
    Set-Content $prefs $existing -Encoding UTF8
    $lines[0] = $lines[0].Replace('=granted', '=denied')
    $lines += 'other.package\:signer\:permission=denied'
    Set-Content $permFile $lines -Encoding ASCII
    Initialize-MicrogDefaults $profile
    if ((Get-Content $prefs -Raw).Trim() -ne $existing) { throw 'Existing microG preferences overwritten' }
    if ((@(Get-Content $permFile) -join "`n") -cne ($lines -join "`n")) { throw 'Existing permission decisions overwritten or duplicated' }
    'PASS: microG defaults use escaped property keys and preserve existing preferences and denials'
} finally {
    # This path is created exclusively for this test beneath the temp directory.
    $full = [IO.Path]::GetFullPath($profile)
    if ((Split-Path $full) -ne [IO.Path]::GetTempPath().TrimEnd('\')) { throw 'Unexpected test cleanup path' }
    if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
}
