# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Real production refresh/notification functions and the actual managed monitor,
# with a queued owner-thread context instead of WPF. Rendering is an explicit fixture
# boundary. The Windows idle suite additionally exercises the real WPF event order.
param([string] $PanelPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-control-panel.ps1'))
$ErrorActionPreference = 'Stop'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile([IO.Path]::GetFullPath($PanelPath), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
foreach ($name in 'Get-FileStamp', 'Get-StateStamp', 'Get-AppFilesStamp', 'Test-PanelMetadataVisible',
        'Request-PanelMetadataRefresh', 'Update-PanelMetadataWatch', 'Refresh-Dashboard',
        'Invoke-PanelMetadataVisibilityChanged', 'Invoke-PanelMetadataActivated', 'Invoke-LiveRefreshTick') {
    $definition = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $false))
    if ($definition.Count -ne 1) { throw "Expected one production function: $name" }
    . ([scriptblock]::Create($definition[0].Extent.Text))
}
# Use the production event subscription's body; do not duplicate its filtering rules.
$handler = @($ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.InvokeMemberExpressionAst] -and
        $node.Member.Value -eq 'add_Changed'
}, $true))
if ($handler.Count -ne 1) { throw 'Expected one production Changed subscription' }
$body = $handler[0].Arguments[0].ScriptBlock.Extent.Text
$changed = [scriptblock]::Create($body.Substring(1, $body.Length - 2))

# Compile the unchanged monitor source without invoking its Windows shell P/Invoke or
# the separately tested on-disk cache. FileSystemWatcher and process observation are real.
$native = Join-Path (Split-Path -Parent ([IO.Path]::GetFullPath($PanelPath))) 'gesher-panel-native.ps1'
$nativeAst = [Management.Automation.Language.Parser]::ParseFile($native, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
$sources = @($nativeAst.FindAll({ param($node)
    $node -is [Management.Automation.Language.StringConstantExpressionAst] -and
        $node.Value.Contains('public sealed class PanelChangeMonitor')
}, $true))
if ($sources.Count -ne 1) { throw 'Expected one production monitor source' }
if (-not ('Gesher.PanelChangeMonitor' -as [type])) { Add-Type -TypeDefinition $sources[0].Value }
if (-not ('PanelStartupContext' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Threading;
public sealed class PanelStartupContext : SynchronizationContext {
    readonly Queue<Action> pending = new Queue<Action>();
    public override void Post(SendOrPostCallback callback, object state) {
        lock (pending) { pending.Enqueue(() => callback(state)); }
    }
    public void Drain() {
        for (int i = 0; i < 100; ++i) {
            Action next;
            lock (pending) { if (pending.Count == 0) return; next = pending.Dequeue(); }
            next();
        }
        throw new InvalidOperationException("Unbounded startup notification loop");
    }
}
'@
}

$script:Checks = 0
function Assert-Startup([bool] $okay, [string] $reason) {
    if (-not $okay) { throw $reason }
    $script:Checks++
}
function Get-DeviceStatus { $false }
function Get-MicrogStatus { $false }
function Update-DeviceView { }
function Update-MicrogView { }
function Update-StoreView { }
function Get-InstalledApps {
    $script:Reads++
    return [IO.File]::ReadAllText($script:PackagesPath)
}
function Update-AppsView($installed) {
    $script:Shown = $installed
    $script:ViewState.AppFiles = $script:ExtraPaths
    if ($script:MutateDuringRender) {
        $script:MutateDuringRender = $false
        $before = [IO.File]::GetLastWriteTimeUtc($script:PackagesPath)
        [IO.File]::WriteAllText($script:PackagesPath, 'second')
        [IO.File]::SetLastWriteTimeUtc($script:PackagesPath, $before)
        # Deterministic actual monitor notification while the UI holds its snapshot.
        # An already-exited/nonexistent observed process uses this same Signal path.
        $script:MetadataMonitor.SetProcess(2147483647)
    }
}

$fixture = Join-Path ([IO.Path]::GetTempPath()) ('gesher-panel-startup-' + [guid]::NewGuid().ToString('N'))
$root = Join-Path $fixture 'profile'
$script:PackagesPath = Join-Path $root 'data/system/packages.list'
$script:AppWindowsFile = Join-Path $root 'data/system/app-windows.properties'
$script:ProxyFile = Join-Path $root 'data/system/proxy.properties'
[void][IO.Directory]::CreateDirectory((Split-Path -Parent $script:PackagesPath))
$script:MetadataMonitor = $null
$script:LiveRefreshDebounceMs = 400; $script:LiveRefreshMaximumDelayMs = 2000; $script:LiveRefreshRecoveryMs = 5000
try {
    foreach ($scenario in 'quiet', 'change-during-read', 'new-watch-after-read') {
        [IO.File]::WriteAllText($script:PackagesPath, 'first!')
        $script:Reads = 0; $script:Shown = $null; $script:IsBusy = $false
        $script:MutateDuringRender = ($scenario -eq 'change-during-read')
        $script:ExtraPaths = @()
        if ($scenario -eq 'new-watch-after-read') {
            $external = Join-Path $fixture 'external/icon.ico'
            [void][IO.Directory]::CreateDirectory((Split-Path -Parent $external))
            [IO.File]::WriteAllText($external, 'icon')
            $script:ExtraPaths = @($external)
        }
        $window = @{ IsVisible = $false; WindowState = 'Normal' }
        $script:ViewState = @{ AppFiles = @() }
        $script:LiveState = @{ Stamp = $null; AppsStamp = $null; PendingSince = $null; PendingFirst = $null;
            Tick = 0; WasVisible = $false; RecoveryAt = $null }
        $context = [PanelStartupContext]::new()
        $script:MetadataMonitor = [Gesher.PanelChangeMonitor]::new($context)
        $script:MetadataMonitor.add_Changed($changed)
        Refresh-Dashboard
        Assert-Startup ($script:Reads -eq 1) "$scenario repeated the initial read"
        if ($scenario -ne 'quiet') {
            Assert-Startup ($null -ne $script:LiveState.PendingSince) "$scenario acknowledged a notification not covered by the rendered snapshot"
        }
        if ($scenario -eq 'change-during-read') {
            Assert-Startup ($script:Shown -ceq 'first!') 'Fixture failed to hold the original snapshot'
            Assert-Startup ((Get-StateStamp) -ceq $script:LiveState.Stamp) 'Fixture did not preserve the recovery stamp'
        }
        $context.Drain()
        $window.IsVisible = $true
        Invoke-PanelMetadataVisibilityChanged
        Invoke-PanelMetadataActivated
        if ($scenario -eq 'quiet') {
            Invoke-LiveRefreshTick
            Assert-Startup ($script:Reads -eq 1 -and -not $script:LiveState.PendingSince) 'Quiet first show performed a redundant refresh'
        } else {
            Assert-Startup ($null -ne $script:LiveState.PendingSince) "$scenario lost dirty state during first show"
            $script:LiveState.PendingSince = [DateTime]::UtcNow.AddSeconds(-1)
            Invoke-LiveRefreshTick
            Assert-Startup ($script:Reads -eq 2) "$scenario was not refreshed before periodic recovery"
            if ($scenario -eq 'change-during-read') {
                Assert-Startup ($script:Shown -ceq 'second') 'Concurrent startup change left the view stale'
            }
        }
        $script:MetadataMonitor.Dispose()
        $script:MetadataMonitor = $null
        $context.Drain()
    }
    Write-Host "PASS: control panel startup snapshots ($script:Checks checks; quiet startup performs one read)"
} finally {
    if ($script:MetadataMonitor) { $script:MetadataMonitor.Dispose() }
    Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction SilentlyContinue
}
