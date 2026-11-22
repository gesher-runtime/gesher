# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# The Control Center's install failures carry their reason: Install-PanelApk (the
# production function, taken from gesher-control-panel.ps1) hands gesher-app.ps1 a
# file in GESHER_LAUNCH_ERROR_FILE, which PowerShell passes to Launcher through
# Unicode arguments. com.gesher.app.Launcher writes why an
# install failed (a different signing key, ...), and shows it after "failed".
# gesher-app.ps1 is a stand-in here, so this runs without a runtime.

param(
    [string] $PanelPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-control-panel.ps1'),
    [string] $UiMessagesPath = (Join-Path $PSScriptRoot '../../../tools/package/ui-messages.json'),
    [string] $AppScriptPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-app.ps1')
)

$ErrorActionPreference = 'Stop'
$script:checks = 0
function Assert-Install([bool] $condition, [string] $message) {
    if (-not $condition) { throw "FAIL: $message" }
    $script:checks++
}

# The texts the launcher and gesher-launch show, in both languages.
$catalog = Get-Content -LiteralPath $UiMessagesPath -Raw -Encoding UTF8 | ConvertFrom-Json
foreach ($key in 'launch_failed', 'install_update_incompatible', 'install_bad_signature',
                 'install_bad_split_signature', 'install_missing_split', 'cp_install_failed') {
    $entry = $catalog.messages.PSObject.Properties[$key]
    Assert-Install ($null -ne $entry) "ui-messages.json has '$key'"
    foreach ($lang in 'en', 'he') {
        Assert-Install (-not [string]::IsNullOrWhiteSpace($entry.Value.$lang)) "'$key' has $lang text"
        Assert-Install ($entry.Value.$lang.Contains('{0}')) "'$key' ($lang) shows its argument"
    }
}
foreach ($lang in 'en', 'he') {
    # The advice names a command that exists (gesher.ps1 runs a dex, not uninstall).
    Assert-Install ($catalog.messages.install_update_incompatible.$lang.Contains('gesher-app uninstall {0}')) "install_update_incompatible ($lang) names gesher-app uninstall"
}
Assert-Install ($catalog.messages.install_update_incompatible.he.Contains($catalog.messages.cp_title.he)) 'The Hebrew advice names the Control Center as its title does'

# Install-PanelApk itself, as the panel defines it.
$ast = [Management.Automation.Language.Parser]::ParseFile((Resolve-Path $PanelPath), [ref]$null, [ref]$null)
$function = $ast.Find({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Install-PanelApk' }, $true)
Assert-Install ($null -ne $function) 'The control panel defines Install-PanelApk'
. ([scriptblock]::Create($function.Extent.Text))
function Get-GesherText([string] $Key, [object[]] $Values = @()) { "[$Key]" + ($Values -join ',') }

$work = Join-Path ([IO.Path]::GetTempPath()) ('gesher-install-errors-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $work | Out-Null
$here = $work
$fake = Join-Path $work 'gesher-app.ps1'
$seen = Join-Path $work 'seen.txt'
$savedErrorCarrier = $env:GESHER_LAUNCH_ERROR_FILE
try {
    # 1. A refused install: the launcher's reason follows the panel's message.
    # Bidi isolates, an em dash and Hebrew (built from code points: this file is ASCII).
    $reason = "The application $([char]0x2068)com.example$([char]0x2069) is already installed`n(different key) " +
        [string][char]0x2014 + ' ' + (-join ([char[]](0x05D4, 0x05D9, 0x05D9, 0x05E9, 0x05D5, 0x05DD)))
    Set-Content -LiteralPath $fake -Encoding UTF8 -Value @"
Set-Content -LiteralPath '$seen' -Value `$env:GESHER_LAUNCH_ERROR_FILE -NoNewline
[IO.File]::WriteAllText(`$env:GESHER_LAUNCH_ERROR_FILE, @'
$reason
'@, [Text.UTF8Encoding]::new(`$false))
exit 1
"@
    $env:GESHER_LAUNCH_ERROR_FILE = 'caller-value'
    $message = $null
    try { Install-PanelApk 'C:\x\app.apk' 'app.apk' } catch { $message = $_.Exception.Message }
    Assert-Install ($null -ne $message) 'A failed install throws'
    Assert-Install ($message.StartsWith('[cp_install_failed]app.apk')) "The message starts with cp_install_failed: $message"
    Assert-Install ($message.Contains($reason)) "The message carries the launcher's reason, UTF-8 intact: $message"
    $errorFile = Get-Content -LiteralPath $seen -Raw
    Assert-Install ($errorFile -and $errorFile -ne 'caller-value') 'gesher-app got a fresh error file'
    Assert-Install (-not (Test-Path -LiteralPath $errorFile)) 'The error file is removed'
    Assert-Install ($env:GESHER_LAUNCH_ERROR_FILE -eq 'caller-value') "The caller's GESHER_LAUNCH_ERROR_FILE is restored"
    Remove-Item Env:GESHER_LAUNCH_ERROR_FILE

    # 2. A failure without a reason: the panel's message alone.
    Set-Content -LiteralPath $fake -Encoding UTF8 -Value "exit 3"
    $message = $null
    try { Install-PanelApk 'C:\x\app.apk' 'app.apk' } catch { $message = $_.Exception.Message }
    Assert-Install ($message -eq '[cp_install_failed]app.apk') "Without a reason only the panel's message: $message"
    Assert-Install ($null -eq $env:GESHER_LAUNCH_ERROR_FILE) 'An unset GESHER_LAUNCH_ERROR_FILE stays unset'

    # 3. gesher-app.ps1 throwing: the error propagates and the file still goes.
    Set-Content -LiteralPath $fake -Encoding UTF8 -Value @"
Set-Content -LiteralPath '$seen' -Value `$env:GESHER_LAUNCH_ERROR_FILE -NoNewline
throw 'script failed'
"@
    $message = $null
    try { Install-PanelApk 'C:\x\app.apk' 'app.apk' } catch { $message = $_.Exception.Message }
    Assert-Install ($message -eq 'script failed') "A throwing gesher-app.ps1 propagates: $message"
    Assert-Install (-not (Test-Path -LiteralPath (Get-Content -LiteralPath $seen -Raw))) 'The error file is removed when gesher-app.ps1 throws'

    # 4. Success: no exception.
    Set-Content -LiteralPath $fake -Encoding UTF8 -Value "exit 0"
    Install-PanelApk 'C:\x\app.apk' 'app.apk'
    Assert-Install $true 'A successful install does not throw'

    # 5. No temporary file: the inner install must not overwrite a caller's file.
    $outerFile = Join-Path $work 'outer-diagnostic.txt'
    [IO.File]::WriteAllText($outerFile, 'outer reason')
    Set-Content -LiteralPath $fake -Encoding UTF8 -Value @"
[IO.File]::WriteAllText('$seen', [string]`$env:GESHER_LAUNCH_ERROR_FILE)
if (`$env:GESHER_LAUNCH_ERROR_FILE) { [IO.File]::WriteAllText(`$env:GESHER_LAUNCH_ERROR_FILE, 'overwritten') }
exit 1
"@
    $savedTemp = @{}
    try {
        foreach ($name in 'TMP', 'TEMP', 'TMPDIR') {
            $savedTemp[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
            [Environment]::SetEnvironmentVariable($name, (Join-Path $work 'missing-temp-directory'), 'Process')
        }
        $creationFailed = $false
        try { $unexpectedFile = [IO.Path]::GetTempFileName(); Remove-Item -LiteralPath $unexpectedFile }
        catch { $creationFailed = $true }
        Assert-Install $creationFailed 'The temporary-file failure control is active'
        $env:GESHER_LAUNCH_ERROR_FILE = $outerFile
        $message = $null
        try { Install-PanelApk 'C:\x\app.apk' 'app.apk' } catch { $message = $_.Exception.Message }
        Assert-Install ($message -eq '[cp_install_failed]app.apk') 'A missing temporary file retains the normal failure'
        Assert-Install ([IO.File]::ReadAllText($seen) -eq '') 'The inner install gets no caller-owned error target'
        Assert-Install ([IO.File]::ReadAllText($outerFile) -eq 'outer reason') 'The caller-owned diagnostic is unchanged'
        Assert-Install ($env:GESHER_LAUNCH_ERROR_FILE -eq $outerFile) 'The caller target is restored after creation failure'
    } finally {
        foreach ($name in 'TMP', 'TEMP', 'TMPDIR') {
            [Environment]::SetEnvironmentVariable($name, $savedTemp[$name], 'Process')
        }
    }

    # 6. Execute the production VM-invocation block with only dalvikvm replaced.
    # This checks both command branches, argument placement and clearing/restoration
    # around actual script invocation, including an exception from the child script.
    $appText = [IO.File]::ReadAllText((Resolve-Path $AppScriptPath))
    $appAst = [Management.Automation.Language.Parser]::ParseInput($appText, [ref]$null, [ref]$null)
    $boundary = $appAst.Find({ param($n)
        $n -is [Management.Automation.Language.AssignmentStatementAst] -and
        $n.Left -is [Management.Automation.Language.VariableExpressionAst] -and
        $n.Left.VariablePath.UserPath -eq 'previousMicrogSpoof'
    }, $true)
    Assert-Install ($null -ne $boundary) 'The production VM invocation boundary exists'
    $bridge = Join-Path $work 'invoke-vm.ps1'
    [IO.File]::WriteAllText($bridge, $appText.Substring($boundary.Extent.StartOffset), [Text.UTF8Encoding]::new($false))
    $bridgeReport = Join-Path $work 'vm-arguments.json'
    $dalvikvm = Join-Path $work 'dalvikvm.ps1'
    Set-Content -LiteralPath $dalvikvm -Encoding UTF8 -Value @'
[pscustomobject]@{ Arguments = @($args); Carrier = $env:GESHER_LAUNCH_ERROR_FILE } |
    ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $bridgeReport -Encoding UTF8
if ($bridgeThrows) { throw 'VM invocation failed' }
exit $bridgeExitCode
'@
    function NoDrive([string] $p) { $p }
    function UserPath([string] $p) { $p }
    $unicodePath = Join-Path $work ('error ' + [string][char]0x05D0 + [char]0x4E2D + '.txt')
    $commands = @('install', 'view', 'launch')
    $dvm = @('-Xint')
    $apkFull = 'C:\path with spaces\app.apk'
    $enableMicrogForProfile = $false
    $bridgeThrows = $false
    $bridgeExitCode = 1
    foreach ($command in 'install', 'app.apk') {
        $rest = @(if ($command -ne 'install') { '--headless' })
        $env:GESHER_LAUNCH_ERROR_FILE = $unicodePath
        & $bridge
        $received = Get-Content -LiteralPath $bridgeReport -Raw -Encoding UTF8 | ConvertFrom-Json
        $expected = @('-Xint', 'com.gesher.app.Launcher', '--launch-error-file', $unicodePath,
            $(if ($command -eq 'install') { 'install' } else { 'launch' }), $apkFull) + $rest
        Assert-Install (($received.Arguments -join "`n") -ceq ($expected -join "`n")) "Unicode path follows the main class without changing command arguments (got: $($received.Arguments -join '|'); expected: $($expected -join '|'))"
        Assert-Install ([string]::IsNullOrEmpty($received.Carrier)) 'The VM does not inherit the environment carrier'
        Assert-Install ($env:GESHER_LAUNCH_ERROR_FILE -ceq $unicodePath) 'The caller carrier is restored after VM exit'
    }
    $bridgeThrows = $true
    $message = $null
    try { & $bridge } catch { $message = $_.Exception.Message }
    Assert-Install ($message -eq 'VM invocation failed') 'VM invocation exceptions propagate'
    Assert-Install ($env:GESHER_LAUNCH_ERROR_FILE -ceq $unicodePath) 'The caller carrier is restored after a VM exception'
    $bridgeThrows = $false
    $bridgeExitCode = 0
    Remove-Item Env:GESHER_LAUNCH_ERROR_FILE
    & $bridge
    $received = Get-Content -LiteralPath $bridgeReport -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-Install ($received.Arguments -notcontains '--launch-error-file') 'An ordinary launch receives no private option'
    Assert-Install ($null -eq $env:GESHER_LAUNCH_ERROR_FILE) 'An absent carrier remains absent after VM success'
} finally {
    if ($null -eq $savedErrorCarrier) { Remove-Item Env:GESHER_LAUNCH_ERROR_FILE -ErrorAction SilentlyContinue }
    else { $env:GESHER_LAUNCH_ERROR_FILE = $savedErrorCarrier }
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
'PASS  shell: install failures carry the launcher''s reason ({0} checks)' -f $script:checks
