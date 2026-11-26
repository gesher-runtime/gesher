# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# The control panel's "Open logs location" runs the production function on a scratch
# profile whose path has spaces: the folder is created when missing and File Explorer
# is started on that exact folder, as one quoted argument.
param(
    [string] $PanelPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-control-panel.ps1')
)

$ErrorActionPreference = 'Stop'
$source = [IO.File]::ReadAllText([IO.Path]::GetFullPath($PanelPath))
$tokens = $null; $parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseInput($source, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
$definitions = @($ast.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Open-PanelLogsFolder'
}, $false))
if ($definitions.Count -ne 1) { throw 'Expected one production function: Open-PanelLogsFolder' }
. ([scriptblock]::Create($definitions[0].Extent.Text))
if ($source -notmatch [regex]::Escape("Open-PanelLogsFolder (Join-Path `$root 'logs')")) {
    throw 'The button must open the profile''s logs folder'
}

# Records what would start instead of opening a window.
$script:Started = [System.Collections.Generic.List[object]]::new()
function Start-Process([string] $FilePath, [string[]] $ArgumentList) {
    $script:Started.Add([pscustomobject]@{ FilePath = $FilePath; ArgumentList = $ArgumentList })
}

$scratch = Join-Path ([IO.Path]::GetTempPath()) ('gesher panel logs ' + [Guid]::NewGuid().ToString('N'))
try {
    $logs = Join-Path $scratch 'profile root/logs'
    Open-PanelLogsFolder $logs
    if (-not [IO.Directory]::Exists($logs)) { throw 'A missing logs folder was not created' }
    if ($script:Started.Count -ne 1) { throw "Expected one start, got $($script:Started.Count)" }
    $start = $script:Started[0]
    if ($start.FilePath -ne (Join-Path $env:WINDIR 'explorer.exe')) { throw "Started $($start.FilePath), not File Explorer" }
    if ($start.ArgumentList.Count -ne 1 -or $start.ArgumentList[0] -cne ('"' + $logs + '"')) {
        throw "File Explorer argument was '$($start.ArgumentList -join ' ')', not the quoted folder"
    }

    # An existing folder (with its logs) is opened unchanged.
    [IO.File]::WriteAllText((Join-Path $logs 'system_server.log'), 'kept')
    Open-PanelLogsFolder $logs
    if ([IO.File]::ReadAllText((Join-Path $logs 'system_server.log')) -cne 'kept') { throw 'An existing log changed' }
    if ($script:Started.Count -ne 2) { throw 'An existing folder was not opened' }

    # A path that cannot be a folder fails, so the panel shows the error instead of opening nothing.
    $blocked = Join-Path $scratch 'file'
    [IO.File]::WriteAllText($blocked, '')
    $failed = $false
    try { Open-PanelLogsFolder (Join-Path $blocked 'logs') } catch { $failed = $true }
    if (-not $failed -or $script:Started.Count -ne 2) { throw 'A folder that cannot be created must fail without starting Explorer' }
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host 'PASS: control panel opens the logs location'
