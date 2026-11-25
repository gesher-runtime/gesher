# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Gesher Control Center (לוח הבקרה של גשר) - GUI Dashboard for apps and Google Play services.

$ErrorActionPreference = 'Stop'
$here = if ($PSScriptRoot) { $PSScriptRoot } elseif ($MyInvocation.MyCommand.Path) { Split-Path -Parent $MyInvocation.MyCommand.Path } else { (Resolve-Path (Join-Path $PWD 'tools/package')).Path }
. (Join-Path $here 'gesher-runtime-files.ps1')
$root = Set-GesherProfileEnvironment $here

# One Control Center. A second start (the Start menu again, or an app's "App info", which
# the system server answers by writing the package here and opening the panel:
# ControlCenter.java) signals the open panel, which comes to the front and takes the
# request on its live-refresh timer, and exits. The mutex is only a name (never owned);
# a panel closes its handles as its window closes, so a start right after that opens a
# new one. "Reopen" after an update starts the new version while the old one is still
# open: GESHER_PANEL_REOPEN lets that start through.
$script:PanelReopen = ($env:GESHER_PANEL_REOPEN -eq '1')
Remove-Item Env:GESHER_PANEL_REOPEN -ErrorAction SilentlyContinue
$script:PanelInstanceCreated = $false
$panelName = Get-GesherControlPanelName $root
$script:PanelInstance = [System.Threading.Mutex]::new($false, $panelName, [ref] $script:PanelInstanceCreated)
$script:PanelShowEvent = [System.Threading.EventWaitHandle]::new($false, [System.Threading.EventResetMode]::AutoReset, ($panelName + '.Show'))
if (-not $script:PanelInstanceCreated -and -not $script:PanelReopen) {
    $null = $script:PanelShowEvent.Set()
    exit 0
}

Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase, System.Drawing

# Set explicit AppUserModelID so Windows taskbar groups under Gesher instead of PowerShell host
try {
    . (Join-Path $here 'gesher-panel-native.ps1')
    [Gesher.Win32ShellGroup]::SetCurrentProcessExplicitAppUserModelID("Gesher.ControlPanel")
} catch { }

. (Join-Path $here 'gesher-ui.ps1')
. (Join-Path $here 'gesher-update.ps1')
. (Join-Path $here 'gesher-panel-view.ps1')
$script:UpdateDirectory = Join-Path $root 'updates'
# Legacy single-file requests are imported for an older running server.
$script:PanelRequestFile = Join-Path $root 'data/system/control-panel-request'
$script:PanelRequestsDir = Join-Path $root 'data/system/control-panel-requests'
# Requests waiting for their result: one file per token, holding the package (ControlCenter.java).
$script:PanelResultsDir = Join-Path $root 'data/system/control-panel-results'
# The package whose details are shown for a request: a new request never replaces its tokens.
$script:PanelShowingFile = Join-Path $root 'data/system/control-panel-showing'
$script:PanelLogFile = Join-Path $root 'logs/control-panel.log'
# Apps whose tasks are windows of their own (ADR 0088; AppWindows.java): separate.<package>=true.
$script:AppWindowsFile = Join-Path $root 'data/system/app-windows.properties'
# Gesher's own proxy (ProxySetting.java, docs/network.md): proxy=<every app>, proxy.<package>=<one app>.
$script:ProxyFile = Join-Path $root 'data/system/proxy.properties'
# Gesher's camera settings: image_picker_fallback=true.
$script:CameraFile = Join-Path $root 'data/system/camera.properties'

$isHe = ($script:GesherUiLanguage -eq 'he')
$flowDirection = if ($isHe) { 'RightToLeft' } else { 'LeftToRight' }

# What the dashboard currently shows ($null = never drawn) and the live-refresh bookkeeping.
# Hashtables are mutated in place so event handlers never need to assign script-scope variables.
$script:IsBusy = $false
$script:ActionState = @{ Worker = $null; Completing = $false; Closed = $false }
$script:ViewState = @{ Device = $null; Microg = $null; Store = $null; AppsSignature = $null; AppFiles = @(); Icons = @{} }
$script:LiveState = @{ Stamp = $null; AppsStamp = $null; PendingSince = $null; PendingFirst = $null; Tick = 0; WasVisible = $false; RecoveryAt = $null }
$script:MetadataMonitor = $null
$script:UpdatingCameraView = $false

# Camera image-picker fallback setting (data/system/camera.properties).
function Get-CameraImagePickerEnabled {
    if ([string]::IsNullOrEmpty($script:CameraFile) -or -not (Test-Path -LiteralPath $script:CameraFile)) { return $false }
    $enabled = $false
    try {
        foreach ($line in Get-Content -LiteralPath $script:CameraFile) {
            if ($line -match '^\s*image_picker_fallback\s*[=:]\s*(.+?)\s*$') {
                $enabled = ($Matches[1].Trim().ToLowerInvariant() -eq 'true')
            }
        }
    } catch { return $false }
    return $enabled
}

function Set-CameraImagePickerEnabled([bool] $enabled) {
    $lines = [System.Collections.Generic.List[string]]::new()
    if (Test-Path -LiteralPath $script:CameraFile) {
        foreach ($line in Get-Content -LiteralPath $script:CameraFile) {
            if ($line -match '^\s*image_picker_fallback\s*[=:]') { continue }
            $lines.Add($line)
        }
    } else {
        $lines.Add('#Gesher camera settings')
    }
    if ($enabled) { $lines.Add('image_picker_fallback=true') }
    $directory = Split-Path -Parent $script:CameraFile
    if (-not (Test-Path -LiteralPath $directory)) { [void](New-Item -ItemType Directory -Path $directory) }
    # The system server reads the file at any time: write a sibling, then move it into place.
    $temporary = $script:CameraFile + '.tmp'
    [IO.File]::WriteAllLines($temporary, $lines, [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporary -Destination $script:CameraFile -Force
}

# Packages whose tasks are windows of their own (AppWindows.java's file).
function Get-SeparateWindowApps {
    $set = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    if (-not (Test-Path -LiteralPath $script:AppWindowsFile)) { return ,$set }
    foreach ($line in Get-Content -LiteralPath $script:AppWindowsFile) {
        if ($line -match '^\s*separate\.([A-Za-z0-9_.]+)\s*[=:]\s*true\s*$') { [void]$set.Add($Matches[1]) }
    }
    return ,$set
}

# Saves whether an app's tasks are windows of their own (from its next start).
function Set-AppSeparateWindows([string] $pkg, [bool] $separate) {
    if ($pkg -cnotmatch '\A[A-Za-z0-9_.]+\z') { return }
    $lines = [System.Collections.Generic.List[string]]::new()
    if (Test-Path -LiteralPath $script:AppWindowsFile) {
        foreach ($line in Get-Content -LiteralPath $script:AppWindowsFile) {
            if ($line -match ('^\s*separate\.' + [regex]::Escape($pkg) + '\s*[=:]')) { continue }
            $lines.Add($line)
        }
    } else {
        $lines.Add('#Gesher''s app windows: gesher-app windows PACKAGE separate|shared')
    }
    if ($separate) { $lines.Add("separate.$pkg=true") }
    $directory = Split-Path -Parent $script:AppWindowsFile
    if (-not (Test-Path -LiteralPath $directory)) { [void](New-Item -ItemType Directory -Path $directory) }
    [IO.File]::WriteAllLines($script:AppWindowsFile, $lines, [Text.UTF8Encoding]::new($false))
}

# A proxy setting as ProxySetting.parse saves it: 'direct', 'windows' or host:port (an
# http:// prefix and a trailing slash dropped); $null when it is not one. The port is read
# as Integer.parseInt reads it (a sign, then decimal digits of any script) so that every
# setting the runtime accepts is shown and saved unchanged.
function ConvertTo-ProxyValue([string] $value) {
    if ($null -eq $value) { return $null }
    # String.trim: every character up to U+0020.
    $v = $value.Trim([char[]](0..32))
    $lower = $v.ToLowerInvariant()
    # Ordinal, as String.equals: -in and -eq would ignore characters such as a soft hyphen.
    if ([Array]::IndexOf([string[]]@('direct', 'none', 'off'), $lower) -ge 0) { return 'direct' }
    if ([Array]::IndexOf([string[]]@('windows', 'default'), $lower) -ge 0) { return 'windows' }
    if ($lower.StartsWith('http://', [StringComparison]::Ordinal)) { $v = $v.Substring(7) }
    if ($v.EndsWith('/', [StringComparison]::Ordinal)) { $v = $v.Substring(0, $v.Length - 1) }
    $colon = $v.LastIndexOf(':')
    if ($colon -le 0 -or $colon -eq $v.Length - 1) { return $null }
    $hostName = $v.Substring(0, $colon)
    if ($hostName -cnotmatch '\A(?:[A-Za-z0-9.\-]+|\[[0-9A-Fa-f:.]+\])\z') { return $null }
    $digits = $v.Substring($colon + 1)
    $start = 0; $negative = $false
    if ($digits[0] -eq [char]'+' -or $digits[0] -eq [char]'-') { $negative = $digits[0] -eq [char]'-'; $start = 1 }
    if ($start -ge $digits.Length) { return $null }
    [long] $port = 0
    for ($i = $start; $i -lt $digits.Length; $i++) {
        if (-not [char]::IsDigit($digits[$i])) { return $null }
        $port = $port * 10 + [long][char]::GetNumericValue($digits[$i])
        if ($port -gt 2147483648) { return $null }   # Integer.parseInt overflows
    }
    if ($negative) { $port = -$port }
    if ($port -lt 1 -or $port -gt 65535) { return $null }
    return $v
}

# proxy.properties as java.util.Properties.load reads it (ISO 8859-1, logical lines with
# '\' continuations, '#'/'!' comments, key/value escapes): its natural lines and, for each
# key/value line, the decoded key and value and the natural lines it spans.
function Read-ProxyProperties {
    $result = @{ Lines = [System.Collections.Generic.List[string]]::new(); Entries = [System.Collections.Generic.List[object]]::new(); Malformed = $false }
    if (-not (Test-Path -LiteralPath $script:ProxyFile)) { return $result }
    # Every byte as one ISO 8859-1 character, as Properties.load(InputStream): ReadAllText
    # would drop a UTF-8 byte order mark that Java reads as part of the first key.
    $text = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($script:ProxyFile))
    foreach ($line in [regex]::Split($text, '\r\n|\r|\n')) { $result.Lines.Add($line) }
    if ($text.Length -gt 0 -and $text[$text.Length - 1] -in [char]"`n", [char]"`r") { $result.Lines.RemoveAt($result.Lines.Count - 1) }
    $blank = [char[]]@(' ', "`t", "`f")
    $i = 0
    while ($i -lt $result.Lines.Count) {
        $first = $i
        $logical = $result.Lines[$i].TrimStart($blank)
        $i++
        if ($logical.Length -eq 0 -or $logical[0] -in [char]'#', [char]'!') { continue }
        # A line ending in an odd number of backslashes continues on the next natural line.
        while (([regex]::Match($logical, '\\*\z').Length % 2) -eq 1) {
            $logical = $logical.Substring(0, $logical.Length - 1)
            if ($i -ge $result.Lines.Count) { break }
            $logical += $result.Lines[$i].TrimStart($blank)
            $i++
        }
        $keyEnd = 0; $escaped = $false
        while ($keyEnd -lt $logical.Length) {
            $c = $logical[$keyEnd]
            if (-not $escaped -and ($c -in [char]'=', [char]':', [char]' ', [char]"`t", [char]"`f")) { break }
            $escaped = (-not $escaped) -and $c -eq [char]'\'
            $keyEnd++
        }
        $valueStart = $keyEnd
        while ($valueStart -lt $logical.Length -and $logical[$valueStart] -in $blank) { $valueStart++ }
        if ($valueStart -lt $logical.Length -and $logical[$valueStart] -in [char]'=', [char]':') { $valueStart++ }
        while ($valueStart -lt $logical.Length -and $logical[$valueStart] -in $blank) { $valueStart++ }
        # Properties.load throws on a \u escape without four hex digits (IllegalArgumentException):
        # ProxySetting.load then has no settings at all, and save does not replace the file.
        foreach ($escape in [regex]::Matches($logical, '\\(u[0-9A-Fa-f]{4}|.)')) {
            if ($escape.Groups[1].Value -ceq 'u') { $result.Malformed = $true }
        }
        $result.Entries.Add([pscustomobject]@{
            Key = ConvertFrom-PropertiesEscapes $logical.Substring(0, $keyEnd)
            Value = ConvertFrom-PropertiesEscapes $logical.Substring($valueStart)
            First = $first; Count = $i - $first
        })
    }
    return $result
}

function ConvertFrom-PropertiesEscapes([string] $text) {
    return [regex]::Replace($text, '\\(u[0-9A-Fa-f]{4}|.)', {
        param($m)
        $e = $m.Groups[1].Value
        switch -CaseSensitive ($e) {
            't' { return "`t" }
            'n' { return "`n" }
            'r' { return "`r" }
            'f' { return "`f" }
            default {
                if ($e.Length -eq 5) { return [string][char][Convert]::ToInt32($e.Substring(1), 16) }
                return $e
            }
        }
    })
}

# A value as java.util.Properties.store writes it.
function ConvertTo-PropertiesValue([string] $text) {
    $out = [Text.StringBuilder]::new()
    for ($i = 0; $i -lt $text.Length; $i++) {
        $c = $text[$i]
        switch -CaseSensitive ([string]$c) {
            '\' { [void]$out.Append('\\'); continue }
            "`t" { [void]$out.Append('\t'); continue }
            "`n" { [void]$out.Append('\n'); continue }
            "`r" { [void]$out.Append('\r'); continue }
            "`f" { [void]$out.Append('\f'); continue }
            default {
                if ($c -eq [char]' ' -and $i -eq 0) { [void]$out.Append('\ ') }
                elseif ($c -in [char]'=', [char]':', [char]'#', [char]'!') { [void]$out.Append('\').Append($c) }
                elseif ([int]$c -lt 0x20 -or [int]$c -gt 0x7E) { [void]$out.Append(('\u{0:X4}' -f [int]$c)) }
                else { [void]$out.Append($c) }
            }
        }
    }
    return $out.ToString()
}

# The saved settings: key 'proxy' (every app) or 'proxy.<package>' -> 'direct', 'windows' or
# host:port. As with Properties, the last line of a key wins; then ProxySetting.parse. None
# when the file is not in Properties form, as ProxySetting.load.
function Get-ProxySettings {
    $settings = [System.Collections.Generic.Dictionary[string, string]]::new([StringComparer]::Ordinal)
    $raw = [System.Collections.Generic.Dictionary[string, string]]::new([StringComparer]::Ordinal)
    $file = Read-ProxyProperties
    if ($file.Malformed) { return ,$settings }
    foreach ($entry in $file.Entries) {
        if ($entry.Key -ceq 'proxy' -or $entry.Key.StartsWith('proxy.', [StringComparison]::Ordinal)) { $raw[$entry.Key] = $entry.Value }
    }
    foreach ($key in $raw.Keys) {
        $value = ConvertTo-ProxyValue $raw[$key]
        if ($value) { $settings[$key] = $value }
    }
    return ,$settings
}

# Saves the proxy of every app ($pkg empty) or of one package, as ProxySetting.save does:
# 'windows' for every app removes the setting; $value $null removes a package's own setting
# (it uses every app's again). App processes started from now on use it.
function Set-ProxySetting([string] $pkg, [string] $value) {
    if ($pkg -and $pkg -cnotmatch '\A[A-Za-z0-9_.]+\z') { throw "Invalid package: $pkg" }
    if ($null -ne $value -and $value -ne '') {
        $value = ConvertTo-ProxyValue $value
        if (-not $value) { throw 'Invalid proxy setting' }
    } else { $value = $null }
    $key = if ($pkg) { "proxy.$pkg" } else { 'proxy' }
    if (-not $pkg -and $value -eq 'windows') { $value = $null }
    $file = Read-ProxyProperties
    # As ProxySetting.save: a file Properties.load cannot read is not replaced.
    if ($file.Malformed) { throw "${script:ProxyFile}: Malformed \uxxxx encoding." }
    $drop = [System.Collections.Generic.HashSet[int]]::new()
    foreach ($entry in $file.Entries) {
        if ($entry.Key -ceq $key) { for ($n = 0; $n -lt $entry.Count; $n++) { [void]$drop.Add($entry.First + $n) } }
    }
    $lines = [System.Collections.Generic.List[string]]::new()
    if (-not (Test-Path -LiteralPath $script:ProxyFile)) {
        $lines.Add('#Gesher''s proxy: gesher-app proxy [PACKAGE] host:port|direct|windows')
    }
    for ($n = 0; $n -lt $file.Lines.Count; $n++) { if (-not $drop.Contains($n)) { $lines.Add($file.Lines[$n]) } }
    if ($value) {
        # A last line ending in an odd number of backslashes would continue into the new one.
        if ($lines.Count -gt 0 -and ([regex]::Match($lines[$lines.Count - 1], '\\*\z').Length % 2) -eq 1) { $lines.Add('') }
        $lines.Add($key + '=' + (ConvertTo-PropertiesValue $value))
    }
    $directory = Split-Path -Parent $script:ProxyFile
    if (-not (Test-Path -LiteralPath $directory)) { [void](New-Item -ItemType Directory -Path $directory) }
    [IO.File]::WriteAllLines($script:ProxyFile, $lines, [Text.Encoding]::GetEncoding(28591))
}

# A saved setting as the panel shows it.
function Get-ProxyDisplayText([string] $value) {
    switch ($value) {
        'direct' { return Get-GesherText 'cp_proxy_mode_direct' }
        'windows' { return Get-GesherText 'cp_proxy_mode_windows' }
        default { return $value }
    }
}

# Fills a mode ComboBox and its address TextBox for a saved value ($null: the package
# uses every app's setting, with -WithInherit; every app's Windows' settings otherwise).
function Initialize-ProxyEditor($combo, $address, [string] $value, [switch] $WithInherit) {
    $combo.Items.Clear()
    $modes = [System.Collections.Generic.List[object]]::new()
    if ($WithInherit) { $modes.Add(@('inherit', (Get-GesherText 'cp_proxy_mode_inherit'))) }
    $modes.Add(@('windows', (Get-GesherText 'cp_proxy_mode_windows')))
    $modes.Add(@('direct', (Get-GesherText 'cp_proxy_mode_direct')))
    $modes.Add(@('server', (Get-GesherText 'cp_proxy_mode_server')))
    foreach ($mode in $modes) {
        $item = [System.Windows.Controls.ComboBoxItem]::new()
        $item.Tag = $mode[0]
        $item.Content = $mode[1]
        [void]$combo.Items.Add($item)
    }
    $selected = if (-not $value) { if ($WithInherit) { 'inherit' } else { 'windows' } }
        elseif ($value -eq 'direct' -or $value -eq 'windows') { $value } else { 'server' }
    $address.Text = if ($selected -eq 'server') { $value } else { '' }
    foreach ($item in $combo.Items) { if ($item.Tag -eq $selected) { $combo.SelectedItem = $item } }
    $address.IsEnabled = ($selected -eq 'server')
    $combo.Add_SelectionChanged({
        param($sender, $e)
        $isServer = ($sender.SelectedItem -and $sender.SelectedItem.Tag -eq 'server')
        $address.IsEnabled = $isServer
        if ($isServer) { [void]$address.Focus() }
    }.GetNewClosure())
}

# The editor's setting: @{ Valid; Value } (Value $null = every app's setting for a package).
function Read-ProxyEditor($combo, $address) {
    $mode = if ($combo.SelectedItem) { [string]$combo.SelectedItem.Tag } else { 'windows' }
    switch ($mode) {
        'inherit' { return @{ Valid = $true; Value = $null } }
        'server' {
            $value = ConvertTo-ProxyValue $address.Text
            if (-not $value -or $value -eq 'direct' -or $value -eq 'windows') {
                return @{ Valid = $false; Value = $null; Text = [string]$address.Text }
            }
            return @{ Valid = $true; Value = $value }
        }
        default { return @{ Valid = $true; Value = $mode } }
    }
}

# Helper to query device running state
function Get-DeviceStatus {
    # The ready file can outlive a crashed device, so the recorded system_server PID must be alive.
    $ready = Join-Path $root 'data/system/gesher-device.ready'
    if (Test-Path -LiteralPath $ready) {
        $text = $null
        try { $text = [string](Get-Content -LiteralPath $ready -Raw -ErrorAction Stop) } catch {
            # The device is rewriting the file; keep the last state shown rather than flicker.
            return [bool]$script:ViewState.Device
        }
        $serverId = 0
        if ([int]::TryParse($text.Trim(), [ref] $serverId)) {
            $p = Get-Process -Id $serverId -ErrorAction SilentlyContinue
            if ($p) {
                try {
                    if ($script:MetadataMonitor) { $script:MetadataMonitor.SetProcess($serverId) }
                } catch { } finally { $p.Dispose() }
                return $true
            }
        }
    }
    try { if ($script:MetadataMonitor) { $script:MetadataMonitor.SetProcess(0) } } catch { }
    return $false
}

# Cheap change detection: length and write time of the files the dashboard derives its state from.
# Notifications normally trigger refresh. These UI-thread stamps recover missed events only
# while the panel is visible; native/.NET callbacks never execute PowerShell off the dispatcher.
function Get-FileStamp([string] $path) {
    try {
        $info = [System.IO.FileInfo]::new($path)
        if ($info.Exists) { return "$($info.Length):$($info.LastWriteTimeUtc.Ticks)" }
    } catch { }
    return '-'
}

function Get-StateStamp {
    return (@(
        Get-FileStamp (Join-Path $root 'data/system/gesher-device.ready')
        Get-FileStamp (Join-Path $root 'data/system/packages.list')
        Get-FileStamp (Join-Path $root 'microg.enabled')
        Get-FileStamp (Join-Path $root 'system/priv-app/com.google.android.gms/base.apk')
        Get-FileStamp $script:AppWindowsFile
        Get-FileStamp $script:ProxyFile
        Get-FileStamp $script:CameraFile
    ) -join '|')
}

# Shortcut labels and icons are written after packages.list, so they are tracked separately.
function Get-AppFilesStamp {
    return (@($script:ViewState.AppFiles | ForEach-Object { Get-FileStamp $_ }) -join '|')
}

function Test-PanelMetadataVisible {
    return $window.IsVisible -and $window.WindowState -ne 'Minimized'
}

function Request-PanelMetadataRefresh {
    # Called on the WPF dispatcher, including notifications marshalled by the monitor.
    $now = [DateTime]::UtcNow
    if (-not $script:LiveState.PendingFirst) { $script:LiveState.PendingFirst = $now }
    $script:LiveState.PendingSince = $now
}

function Update-PanelMetadataWatch {
    if (-not $script:MetadataMonitor) { return }
    $paths = @(
        (Join-Path $root 'data/system/gesher-device.ready')
        (Join-Path $root 'data/system/packages.list')
        (Join-Path $root 'microg.enabled')
        (Join-Path $root 'system/priv-app/com.google.android.gms/base.apk')
        $script:AppWindowsFile
        $script:ProxyFile
        $(if (-not [string]::IsNullOrEmpty($script:CameraFile)) { $script:CameraFile })
    ) + @($script:ViewState.AppFiles)
    $script:MetadataMonitor.SetFiles($root, [string[]]$paths)
}

function Invoke-PanelMetadataVisibilityChanged {
    if (-not (Test-PanelMetadataVisible)) {
        $script:LiveState.InitialShowPending = $false
        $script:LiveState.WasVisible = $false
        return
    }
    if ($script:LiveState.InitialShowPending) {
        $script:LiveState.WasVisible = $true
        return
    }
    # Restore resynchronizes even if the watcher overflowed or a directory was replaced.
    $script:LiveState.WasVisible = $false
    Request-PanelMetadataRefresh
}

function Invoke-PanelMetadataActivated {
    try {
        if ($script:LiveState.InitialShowPending -and (Test-PanelMetadataVisible)) {
            $script:LiveState.InitialShowPending = $false
            $script:LiveState.WasVisible = $true
            return
        }
        Invoke-PanelMetadataVisibilityChanged
        if (-not $script:IsBusy -and (Test-PanelMetadataVisible)) {
            $script:LiveState.WasVisible = $true
            Refresh-Dashboard
        }
    } catch { }
}

function Initialize-PanelMetadata {
    try {
        $context = [System.Windows.Threading.DispatcherSynchronizationContext]::new($window.Dispatcher,
            [System.Windows.Threading.DispatcherPriority]::Background)
        $script:MetadataMonitor = [Gesher.PanelChangeMonitor]::new($context)
        $script:MetadataMonitor.add_Changed({
            try {
                if ($null -ne $script:LiveState.IgnoreChangedGeneration) {
                    $ignored = $script:LiveState.IgnoreChangedGeneration
                    $script:LiveState.IgnoreChangedGeneration = $null
                    if ($script:MetadataMonitor -and $script:MetadataMonitor.Generation -eq $ignored) { return }
                }
                Request-PanelMetadataRefresh
            } catch { }
        })
    } catch { $script:MetadataMonitor = $null }
    $window.Add_StateChanged({ Invoke-PanelMetadataVisibilityChanged })
    $window.Add_IsVisibleChanged({ Invoke-PanelMetadataVisibilityChanged })
    $window.Add_Activated({ Invoke-PanelMetadataActivated })
    $window.Add_Closed({ Close-PanelMetadata })
}

function Close-PanelMetadata {
    if ($script:MetadataMonitor) {
        $script:MetadataMonitor.Dispose()
        $script:MetadataMonitor = $null
    }
}

# Helper to query microG enabled state
function Get-MicrogStatus {
    $marker = Join-Path $root 'microg.enabled'
    $gmsApk = Join-Path $root 'system/priv-app/com.google.android.gms/base.apk'
    return (Test-Path -LiteralPath $marker) -and (Test-Path -LiteralPath $gmsApk)
}

# Helper to read installed applications
function Get-InstalledApps {
    $apps = [System.Collections.Generic.List[PSObject]]::new()
    $packagesFile = Join-Path $root 'data/system/packages.list'
    if (-not (Test-Path -LiteralPath $packagesFile)) { return $apps }

    $lines = Get-Content -LiteralPath $packagesFile
    foreach ($line in $lines) {
        if ([string]::IsNullOrWhiteSpace($line) -or $line.StartsWith('#')) { continue }
        $parts = $line -split "`t"
        if ($parts.Count -lt 1) { continue }
        $pkg = $parts[0].Trim()
        if ([string]::IsNullOrEmpty($pkg)) { continue }

        $label = $pkg
        $iconPath = $null
        # Keep declarations even when unavailable: a later icon creation must
        # trigger observation/recovery without another shortcut rewrite.
        $iconCandidates = [System.Collections.Generic.List[string]]::new()
        $propFile = Join-Path $root "data/data/$pkg/gesher-shell/shortcut.properties"
        if (Test-Path -LiteralPath $propFile) {
            foreach ($propLine in Get-Content -LiteralPath $propFile) {
                if ($propLine -match '^\s*label\s*=\s*(.*)$') {
                    $raw = $Matches[1].Trim()
                    $label = [regex]::Replace($raw, '\\u([0-9a-fA-F]{4})', { param($m) [char][int]"0x$($m.Groups[1].Value)" })
                }
                if ($propLine -match '^\s*icon\s*=\s*(.*)$') {
                    $iconCandidate = $Matches[1].Trim() -replace '\\\\', '\' -replace '\\:', ':'
                    if (-not [string]::IsNullOrWhiteSpace($iconCandidate)) {
                        $iconCandidates.Add($iconCandidate)
                        if (Test-Path -LiteralPath $iconCandidate) { $iconPath = $iconCandidate }
                    }
                }
            }
        }
        if (-not $iconPath) {
            $candidate = Join-Path $root "data/data/$pkg/gesher-shell/icon.ico"
            if (Test-Path -LiteralPath $candidate) { $iconPath = $candidate }
        }

        # As a launcher's app list, "Launch" only for a package with a MAIN/LAUNCHER activity:
        # AppShell records one without (com.gesher.camera's capture activity, for example).
        $launchable = -not (Test-Path -LiteralPath (Join-Path $root "data/data/$pkg/gesher-shell/no-launcher-activity"))

        $apps.Add([PSCustomObject]@{
            Package = $pkg
            Label = $label
            IconPath = $iconPath
            Launchable = $launchable
            IconCandidates = $iconCandidates.ToArray()
        })
    }
    return $apps
}

# Build WPF Window in XAML
$window = New-PanelView 'dashboard' @{ PanelFlowDirection = [System.Windows.FlowDirection]$flowDirection }
$window.MaxHeight = [System.Windows.SystemParameters]::WorkArea.Height
$window.MinHeight = [Math]::Min($window.MinHeight, $window.MaxHeight)
$window.Height = [Math]::Min($window.Height, $window.MaxHeight)

function Get-PanelWindowIcon([string] $path) {
    $stream = [IO.File]::OpenRead($path)
    try {
        $icon = [System.Windows.Media.Imaging.BitmapFrame]::Create($stream,
            [System.Windows.Media.Imaging.BitmapCreateOptions]::None,
            [System.Windows.Media.Imaging.BitmapCacheOption]::OnLoad)
        $icon.Freeze()
        return $icon
    } finally { $stream.Dispose() }
}

# Load the icon completely: setup must be able to replace it while this panel
# monitors the installer. The Uri overload would retain an on-demand file stream.
$icoFile = Join-Path $here 'gesher.ico'
if (Test-Path -LiteralPath $icoFile) {
    try {
        $window.Icon = Get-PanelWindowIcon $icoFile
    } catch { }
}

# Control references
$deviceIndicator = $window.FindName('DeviceIndicator')
$deviceStatusText = $window.FindName('DeviceStatusText')
$btnToggleDevice = $window.FindName('BtnToggleDevice')
$microgBadge = $window.FindName('MicrogBadge')
$microgBadgeText = $window.FindName('MicrogBadgeText')
$btnEnableMicrog = $window.FindName('BtnEnableMicrog')
$btnGoogleLogin = $window.FindName('BtnGoogleLogin')
$btnMicrogSettings = $window.FindName('BtnMicrogSettings')
$btnDisableMicrog = $window.FindName('BtnDisableMicrog')
$storeBadge = $window.FindName('StoreBadge')
$storeBadgeText = $window.FindName('StoreBadgeText')
$btnInstallStore = $window.FindName('BtnInstallStore')
$btnOpenStore = $window.FindName('BtnOpenStore')
$btnInstallApk = $window.FindName('BtnInstallApk')
$appsList = $window.FindName('AppsList')
$statusMessage = $window.FindName('StatusMessage')
$activityBar = $window.FindName('ActivityBar')
$servicesCards = $window.FindName('ServicesCards')
$applicationsSection = $window.FindName('ApplicationsSection')
$btnProxyAll = $window.FindName('BtnProxyAll')
$updateVersionText = $window.FindName('UpdateVersionText')
$updateStatusText = $window.FindName('UpdateStatusText')
$automaticUpdateChecks = $window.FindName('AutomaticUpdateChecks')
$cameraImagePickerFallback = $window.FindName('CameraImagePickerFallback')
$btnCheckUpdate = $window.FindName('BtnCheckUpdate')
$btnInstallUpdate = $window.FindName('BtnInstallUpdate')
$btnCancelUpdate = $window.FindName('BtnCancelUpdate')
$btnUpdateNotes = $window.FindName('BtnUpdateNotes')
$btnReopenPanel = $window.FindName('BtnReopenPanel')
$updateProgress = $window.FindName('UpdateProgress')
$panelScroll = $window.FindName('PanelScroll')
$panelLayout = $window.FindName('PanelLayout')

function Update-PanelWindowLayout {
    # A short or scaled display can scroll the complete dashboard. Its app list
    # keeps a useful viewport rather than being squeezed behind the update and camera cards.
    $panelLayout.Height = [Math]::Max(740, $panelScroll.ActualHeight - 48)
}

function Set-Busy([bool] $busy, [string] $msg = '') {
    # While an action runs (including its nested MessageBox loops) live refresh stays out of the way;
    # every action ends with a full Refresh-Dashboard before it clears the busy state.
    $script:IsBusy = $busy
    $btnToggleDevice.IsEnabled = -not $busy
    $servicesCards.IsEnabled = -not $busy
    $applicationsSection.IsEnabled = -not $busy
    $window.AllowDrop = -not $busy
    $btnInstallUpdate.IsEnabled = -not $busy
    $btnReopenPanel.IsEnabled = -not $busy
    if ($busy) {
        $activityBar.Visibility = [System.Windows.Visibility]::Visible
        $activityBar.IsIndeterminate = $true
        $statusMessage.Text = if ($msg) { $msg } else { Get-GesherText 'cp_status_working' }
    } else {
        $activityBar.Visibility = [System.Windows.Visibility]::Hidden
        $activityBar.IsIndeterminate = $false
        $statusMessage.Text = if ($msg) { $msg } else { Get-GesherText 'cp_status_ready' }
    }
}

# Each Update-* part redraws its controls only when the state it shows actually changed.
function Update-DeviceView([bool] $deviceRunning) {
    if ($script:ViewState.Device -eq $deviceRunning) { return }
    $script:ViewState.Device = $deviceRunning
    if ($deviceRunning) {
        $deviceIndicator.Background = [System.Windows.Media.BrushConverter]::new().ConvertFromString('#12B76A')
        $deviceStatusText.Text = Get-GesherText 'cp_device_running'
        $btnToggleDevice.Content = Get-GesherText 'cp_btn_stop'
    } else {
        $deviceIndicator.Background = [System.Windows.Media.BrushConverter]::new().ConvertFromString('#98A2B3')
        $deviceStatusText.Text = Get-GesherText 'cp_device_stopped'
        $btnToggleDevice.Content = Get-GesherText 'cp_btn_start'
    }
}

function Update-MicrogView([bool] $microgEnabled) {
    if ($script:ViewState.Microg -eq $microgEnabled) { return }
    $script:ViewState.Microg = $microgEnabled
    if ($microgEnabled) {
        $microgBadge.Background = [System.Windows.Media.BrushConverter]::new().ConvertFromString('#ECFDF3')
        $microgBadgeText.Foreground = [System.Windows.Media.BrushConverter]::new().ConvertFromString('#027A48')
        $microgBadgeText.Text = Get-GesherText 'cp_microg_enabled'
        $btnEnableMicrog.Visibility = [System.Windows.Visibility]::Collapsed
        $btnGoogleLogin.Visibility = [System.Windows.Visibility]::Visible
        $btnMicrogSettings.Visibility = [System.Windows.Visibility]::Visible
        $btnDisableMicrog.Visibility = [System.Windows.Visibility]::Visible
    } else {
        $microgBadge.Background = [System.Windows.Media.BrushConverter]::new().ConvertFromString('#F2F4F7')
        $microgBadgeText.Foreground = [System.Windows.Media.BrushConverter]::new().ConvertFromString('#475467')
        $microgBadgeText.Text = Get-GesherText 'cp_microg_disabled'
        $btnEnableMicrog.Visibility = [System.Windows.Visibility]::Visible
        $btnGoogleLogin.Visibility = [System.Windows.Visibility]::Collapsed
        $btnMicrogSettings.Visibility = [System.Windows.Visibility]::Collapsed
        $btnDisableMicrog.Visibility = [System.Windows.Visibility]::Collapsed
    }
}

function Update-StoreView($installed) {
    $hasStore = [bool]($installed | Where-Object { $_.Package -eq 'com.aurora.store' })
    if ($script:ViewState.Store -eq $hasStore) { return }
    $script:ViewState.Store = $hasStore
    if ($hasStore) {
        $storeBadge.Background = [System.Windows.Media.BrushConverter]::new().ConvertFromString('#ECFDF3')
        $storeBadgeText.Foreground = [System.Windows.Media.BrushConverter]::new().ConvertFromString('#027A48')
        $storeBadgeText.Text = Get-GesherText 'cp_store_installed'
        $btnInstallStore.Visibility = [System.Windows.Visibility]::Collapsed
        $btnOpenStore.Visibility = [System.Windows.Visibility]::Visible
    } else {
        $storeBadge.Background = [System.Windows.Media.BrushConverter]::new().ConvertFromString('#F2F4F7')
        $storeBadgeText.Foreground = [System.Windows.Media.BrushConverter]::new().ConvertFromString('#475467')
        $storeBadgeText.Text = Get-GesherText 'cp_store_not_installed'
        $btnInstallStore.Visibility = [System.Windows.Visibility]::Visible
        $btnOpenStore.Visibility = [System.Windows.Visibility]::Collapsed
    }
}

# Share frozen decoded images within the current view and across unchanged refreshes.
function Get-PanelAppIcon([string] $path, [string] $stamp, [hashtable] $icons) {
    if ($icons.ContainsKey($path)) { return $icons[$path].Image }
    $previous = $script:ViewState.Icons[$path]
    if ($previous -and $previous.Stamp -ceq $stamp) {
        $image = $previous.Image
    } else {
        $image = [System.Windows.Media.Imaging.BitmapImage]::new()
        $image.BeginInit()
        $image.UriSource = [System.Uri]::new($path)
        # WPF's process-wide URI cache can otherwise return the old icon even
        # after its write-time/length stamp changed. OnLoad releases the file.
        $image.CreateOptions = [System.Windows.Media.Imaging.BitmapCreateOptions]::IgnoreImageCache
        $image.CacheOption = [System.Windows.Media.Imaging.BitmapCacheOption]::OnLoad
        $image.EndInit()
        $image.Freeze()
    }
    $icons[$path] = @{ Stamp = $stamp; Image = $image }
    return $image
}

# Rebuild the list only when its contents change, keeping unchanged icons decoded.
function Update-AppsView($installed) {
    $appFiles = [System.Collections.Generic.List[string]]::new()
    $watched = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $iconStamps = @{}
    $defaultIco = if (Test-Path -LiteralPath $icoFile) { $icoFile } else { $null }
    $iconStamps[$icoFile] = Get-FileStamp $icoFile
    $appFiles.Add($icoFile)
    [void]$watched.Add($icoFile)
    $signature = @(foreach ($app in $installed) {
        $shell = Join-Path $root "data/data/$($app.Package)/gesher-shell"
        $paths = @((Join-Path $shell 'shortcut.properties'), (Join-Path $shell 'icon.ico'), (Join-Path $shell 'no-launcher-activity'), $app.IconPath)
        if ($app.PSObject.Properties['IconCandidates']) { $paths += @($app.IconCandidates) }
        foreach ($path in $paths) {
            if ($path -and $watched.Add([string]$path)) { $appFiles.Add([string]$path) }
        }
        if ($app.IconPath -and -not $iconStamps.ContainsKey($app.IconPath)) {
            $iconStamps[$app.IconPath] = Get-FileStamp $app.IconPath
        }
        "$($app.Package)`t$($app.Label)`t$($app.IconPath)`t$(if ($app.IconPath) { $iconStamps[$app.IconPath] })`t$($app.Launchable)"
    }) -join "`n"
    $signature += "`n$($iconStamps[$icoFile])"
    $separateApps = Get-SeparateWindowApps
    $signature += "`n" + (@($separateApps | Sort-Object) -join ',')
    $proxySettings = Get-ProxySettings
    $signature += "`n" + (@($proxySettings.Keys | Sort-Object | ForEach-Object { "$_=$($proxySettings[$_])" }) -join ',')
    $script:ViewState.AppFiles = $appFiles.ToArray()
    if ($script:ViewState.AppsSignature -ceq $signature) { return }

    $items = [System.Collections.Generic.List[PSObject]]::new()
    $icons = @{}
    foreach ($app in $installed) {
        $imgSource = $null
        $resolvedIco = if ($app.IconPath -and (Test-Path -LiteralPath $app.IconPath)) { $app.IconPath } else { $defaultIco }
        if ($resolvedIco) {
            try {
                $imgSource = Get-PanelAppIcon $resolvedIco $iconStamps[$resolvedIco] $icons
            } catch { }
        }
        $appProxy = $null
        $proxyText = if ($proxySettings.TryGetValue("proxy.$($app.Package)", [ref] $appProxy)) {
            Get-GesherText 'cp_proxy_app_value' @((Get-ProxyDisplayText $appProxy))
        } else { '' }
        $items.Add([PSCustomObject]@{
            Package = $app.Package
            Label = $app.Label
            IconImage = $imgSource
            SeparateWindows = $separateApps.Contains([string]$app.Package)
            LaunchVisibility = if ($app.Launchable) { [System.Windows.Visibility]::Visible } else { [System.Windows.Visibility]::Collapsed }
            ProxyText = $proxyText
            ProxyVisibility = if ($proxyText) { [System.Windows.Visibility]::Visible } else { [System.Windows.Visibility]::Collapsed }
        })
    }
    $appsList.ItemsSource = $items
    # Retain only icons used by the current view; removed apps release their entries.
    $script:ViewState.Icons = $icons
    $script:ViewState.AppsSignature = $signature
}

# Full re-evaluation; used at load, after every panel action and when live refresh sees a change.
function Refresh-Dashboard {
    # Subscribe before the first snapshot. Only the setup notification already covered
    # by that snapshot may be ignored; changes during the read must remain dirty.
    if ($script:MetadataMonitor -and $script:MetadataMonitor.WatcherCount -eq 0) {
        Update-PanelMetadataWatch
        if ($script:MetadataMonitor.WatcherCount -gt 0) {
            $script:LiveState.IgnoreChangedGeneration = $script:MetadataMonitor.Generation
        }
    }
    $generation = if ($script:MetadataMonitor) { $script:MetadataMonitor.Generation } else { 0 }
    # Stamps precede reads so notifications or visible recovery notice a change during the read.
    $stateStamp = Get-StateStamp
    $appsStamp = Get-AppFilesStamp
    # A transient read/render failure must leave a retry pending even when the files have
    # stopped changing. Commit the observed stamps only after the whole refresh succeeds.
    if (-not $script:LiveState.PendingSince) { Request-PanelMetadataRefresh }
    Update-DeviceView (Get-DeviceStatus)
    Update-MicrogView (Get-MicrogStatus)
    if (Get-Command Update-CameraView -ErrorAction SilentlyContinue) { Update-CameraView }
    $installed = Get-InstalledApps
    Update-StoreView $installed
    Update-AppsView $installed
    Update-PanelMetadataWatch
    if ($null -eq $script:LiveState.Stamp -and -not (Test-PanelMetadataVisible)) {
        $script:LiveState.InitialShowPending = $true
    }
    $script:LiveState.Stamp = $stateStamp
    $script:LiveState.AppsStamp = $appsStamp
    $script:LiveState.RecoveryAt = [DateTime]::UtcNow.AddMilliseconds($script:LiveRefreshRecoveryMs)
    # Do not acknowledge an event arriving during the read or a watch replacement gap.
    if ($script:MetadataMonitor -and $script:MetadataMonitor.Generation -ne $generation) {
        Request-PanelMetadataRefresh
    } else { $script:LiveState.PendingSince = $null; $script:LiveState.PendingFirst = $null }
}

# Always release the action guard: a transient refresh failure is retried by the timer.
function Complete-DashboardAction {
    try { Refresh-Dashboard } finally { Set-Busy $false }
}

# The shared UI heartbeat still services activation, actions and update progress.
# Metadata uses debounced notifications plus visible-only missed-event recovery.
# A hidden/minimized view does no periodic file/PID/icon scans, even when marked dirty.
$script:LiveRefreshIntervalMs = 500
$script:LiveRefreshDebounceMs = 400
$script:LiveRefreshMaximumDelayMs = 2000
$script:LiveRefreshRecoveryMs = 5000

function Invoke-LiveRefreshTick {
    $live = $script:LiveState
    $live.Tick++
    if (-not (Test-PanelMetadataVisible)) { $live.WasVisible = $false; return }
    if ($script:IsBusy) { return }
    if (-not $live.WasVisible) {
        $live.WasVisible = $true
        Refresh-Dashboard
        return
    }
    $live.InitialShowPending = $false
    $now = [DateTime]::UtcNow
    if (-not $live.RecoveryAt -or $now -ge $live.RecoveryAt) {
        $live.RecoveryAt = $now.AddMilliseconds($script:LiveRefreshRecoveryMs)
        $stamp = Get-StateStamp
        $appsStamp = Get-AppFilesStamp
        Update-DeviceView (Get-DeviceStatus)
        if (($stamp -ne $live.Stamp -or $appsStamp -ne $live.AppsStamp) -and -not $live.PendingSince) {
            Request-PanelMetadataRefresh
        }
        if ($script:MetadataMonitor -and -not $script:MetadataMonitor.Healthy) { Update-PanelMetadataWatch }
    }
    if ($live.PendingSince -and (($now - $live.PendingSince).TotalMilliseconds -ge $script:LiveRefreshDebounceMs -or
            ($live.PendingFirst -and ($now - $live.PendingFirst).TotalMilliseconds -ge $script:LiveRefreshMaximumDelayMs))) {
        Refresh-Dashboard
    }
}

function Invoke-PanelHeartbeat {
    if ($script:ActionState.Closed) { return }
    try { Complete-PanelAction } catch { }
    try { Clear-PanelSpecialAccessWorkers } catch { }
    try { Invoke-PanelRequest } catch { }
    try { Invoke-PanelUpdateTick } catch {
        Set-PanelUpdateStatus 'cp_update_check_failed' @() $_.Exception.Message
        Update-PanelUpdateView
    }
    try { Invoke-LiveRefreshTick } catch { }
}

# Takes the system server's request (one line: a package, or empty for the panel itself).
# Moving it aside first means one panel shows it once, even with two panels open (the
# old and new one around "Reopen").
function Read-PanelRequest {
    $file = $script:PanelRequestFile
    if (-not (Test-Path -LiteralPath $file)) { return $null }
    # Reopen can briefly leave two consumers. Each owns its taken file, so a newer
    # request cannot overwrite one that the other panel has moved but not yet read.
    $taken = "$file.$([guid]::NewGuid().ToString('N')).taken"
    try { Move-Item -LiteralPath $file -Destination $taken -ErrorAction Stop } catch { return $null }
    try { $text = [System.IO.File]::ReadAllText($taken) } catch { $text = $null }
    Remove-Item -LiteralPath $taken -Force -ErrorAction SilentlyContinue
    if ($null -eq $text) { return $null }
    return $text.Trim()
}

# The package and result token are one immutable record (ControlCenter.publishRequest).
# A legacy single-line record has no embedded token; its old result files are taken
# only after its details become the owned visit.
function ConvertFrom-QueuedPanelRequest([string] $text) {
    if ($null -eq $text -or $text.Length -gt 1024) { return $null }
    $lines = @($text -split "`r?`n")
    while ($lines.Count -gt 1 -and $lines[-1] -eq '') { $lines = @($lines[0..($lines.Count - 2)]) }
    if ($lines.Count -gt 2) { return $null }
    $package = ConvertFrom-PanelRequest $lines[0]
    $token = if ($lines.Count -eq 2) { $lines[1] } else { '' }
    if ($token -and ($token -cnotmatch '^[0-9a-f]{32}$' -or -not $package)) { return $null }
    return [PSCustomObject]@{
        Package = $(if ($package) { $package.Package } else { $null })
        Tokens = [string[]]$(if ($token) { @($token) } else { @() })
    }
}

# Imports the old server's one-file protocol without a peek/take split. The renamed
# file is now immutable and belongs to the queue even when a different app is shown.
function Import-LegacyPanelRequest {
    if (-not (Test-Path -LiteralPath $script:PanelRequestFile)) { return }
    if (-not (Test-Path -LiteralPath $script:PanelRequestsDir)) {
        [void](New-Item -ItemType Directory -Path $script:PanelRequestsDir -Force)
    }
    if (@(Get-ChildItem -LiteralPath $script:PanelRequestsDir -Filter '*.request' -File).Count -ge 32) { return }
    $name = '{0:D19}-legacy-{1}.request' -f [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds(), [guid]::NewGuid().ToString('N')
    $owned = Join-Path $script:PanelRequestsDir $name
    try { Move-Item -LiteralPath $script:PanelRequestFile -Destination $owned -ErrorAction Stop } catch { }
}

# Claims one complete request; filtering reads an immutable filename, so another
# publisher cannot replace the inspected package with a different package's token.
# Reopen can briefly create two consumers: only the successful rename owns a visit.
function Receive-PanelRequest([string] $Package = '') {
    Import-LegacyPanelRequest
    if (-not (Test-Path -LiteralPath $script:PanelRequestsDir)) { return $null }
    foreach ($file in @(Get-ChildItem -LiteralPath $script:PanelRequestsDir -Filter '*.request' -File | Sort-Object Name)) {
        try { $request = ConvertFrom-QueuedPanelRequest ([IO.File]::ReadAllText($file.FullName)) } catch { continue }
        if ($Package -and (-not $request -or $request.Package -cne $Package)) { continue }
        $taken = "$($file.FullName).$([guid]::NewGuid().ToString('N')).taken"
        try { Move-Item -LiteralPath $file.FullName -Destination $taken -ErrorAction Stop } catch { continue }
        # The selected record is immutable. The successful rename owns the parsed
        # value too; a second read adds I/O and can lose the visit on a transient error.
        Remove-Item -LiteralPath $taken -Force -ErrorAction SilentlyContinue
        # Malformed legacy records still raise the main panel, as the old protocol did.
        if (-not $request) { $request = [PSCustomObject]@{ Package = $null; Tokens = [string[]]@() } }
        return $request
    }
    return $null
}

# packages.list's install and update times (milliseconds since 1970, UTC), as local dates.
function Get-AppInstallTimes([string] $pkg) {
    $packagesFile = Join-Path $root 'data/system/packages.list'
    if (-not (Test-Path -LiteralPath $packagesFile)) { return $null }
    foreach ($line in Get-Content -LiteralPath $packagesFile) {
        $parts = $line -split "`t"
        if ($parts.Count -lt 4 -or $parts[0].Trim() -ne $pkg) { continue }
        $times = @()
        foreach ($value in $parts[2], $parts[3]) {
            $ms = 0L
            if ([long]::TryParse($value.Trim(), [ref] $ms) -and $ms -gt 0) {
                $times += [DateTimeOffset]::FromUnixTimeMilliseconds($ms).LocalDateTime.ToString('g')
            } else { $times += $null }
        }
        return $times
    }
    return $null
}

# Android Settings' special app access switches the App info shows (SpecialAccess.java):
# the app-op's public name and its label. The device lists only those the app requests.
$script:SpecialAccessLabels = [ordered]@{
    'android:system_alert_window' = 'cp_access_overlay'
    'android:request_install_packages' = 'cp_access_install'
    'android:write_settings' = 'cp_access_write_settings'
    'android:get_usage_stats' = 'cp_access_usage'
}
$script:SpecialAccessTimeoutMs = 120000
# Background queries whose dialog closed before they answered; the live timer disposes them.
$script:OrphanSpecialAccessWorkers = [Collections.Generic.List[object]]::new()

# The device's answer (gesher-app special-access): "special-access<TAB>op<TAB>allowed|denied"
# lines, with "<TAB>fixed" when the switch cannot change it, among the launcher's other
# output. Unknown switches are left out.
function ConvertFrom-SpecialAccessOutput([string[]] $lines) {
    $switches = [Collections.Generic.List[object]]::new()
    foreach ($line in $lines) {
        if ($line -cmatch '^special-access\t(android:[a-z_]+)\t(allowed|denied)(\tfixed)?\s*$' -and
                @($script:SpecialAccessLabels.Keys) -ccontains $Matches[1]) {
            $switches.Add([PSCustomObject]@{ Op = $Matches[1]; Allowed = ($Matches[2] -eq 'allowed'); Fixed = [bool]$Matches[3] })
        }
    }
    return $switches.ToArray()
}

# Runs gesher-app special-access in its own process (the script ends with exit), hidden;
# the device authenticates it as the host shell. Returns its exit code and output lines.
# Runs in a background runspace for the dialog (Start-PanelSpecialAccess): never on the UI thread.
function Invoke-PanelSpecialAccess([string[]] $arguments,
        [ValidateSet('special-access', 'settings-shown', 'settings-closed')] [string] $command = 'special-access') {
    $appScript = Join-Path $here 'gesher-app.ps1'
    $system = [Environment]::GetFolderPath('System')
    $info = [System.Diagnostics.ProcessStartInfo]::new((Join-Path $system 'WindowsPowerShell\v1.0\powershell.exe'))
    # Every argument was checked against a fixed pattern: no quoting is needed.
    $info.Arguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + $appScript + '" ' + $command + ' ' + ($arguments -join ' ')
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::Start($info)
    try {
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($script:SpecialAccessTimeoutMs)) {
            # The launcher's dalvikvm is a child of this PowerShell: end the whole tree.
            try {
                $kill = [System.Diagnostics.ProcessStartInfo]::new((Join-Path $system 'taskkill.exe'), "/PID $($process.Id) /T /F")
                $kill.UseShellExecute = $false; $kill.CreateNoWindow = $true
                $killer = [System.Diagnostics.Process]::Start($kill)
                $null = $killer.WaitForExit(10000); $killer.Dispose()
            } catch { }
            try { if (-not $process.HasExited) { $process.Kill() } } catch { }
            return [PSCustomObject]@{ ExitCode = -1; Lines = @() }
        }
        $process.WaitForExit()
        $null = $errors.Result
        return [PSCustomObject]@{ ExitCode = $process.ExitCode; Lines = @($output.Result -split "`r?`n") }
    } finally { $process.Dispose() }
}

# Starts Invoke-PanelSpecialAccess in a background runspace; poll it with
# Test-PanelSpecialAccessDone and take its answer with Receive-PanelSpecialAccess.
function Start-PanelSpecialAccess([string[]] $arguments, [string] $command = 'special-access') {
    $powerShell = [PowerShell]::Create()
    try {
        $body = "param(`$here, `$timeout, `$arguments, `$command)`n`$script:SpecialAccessTimeoutMs = `$timeout`n" +
            "function Invoke-PanelSpecialAccess {`n$((Get-Command Invoke-PanelSpecialAccess).Definition)`n}`n" +
            "Invoke-PanelSpecialAccess `$arguments `$command"
        $null = $powerShell.AddScript($body).AddArgument($here).AddArgument($script:SpecialAccessTimeoutMs).AddArgument([string[]]$arguments).AddArgument($command)
        return [PSCustomObject]@{ PowerShell = $powerShell; Handle = $powerShell.BeginInvoke() }
    } catch {
        $powerShell.Dispose()
        throw
    }
}

function Test-PanelSpecialAccessDone($worker) {
    return [bool]$worker.Handle.IsCompleted
}

# A diagnostic line in logs\control-panel.log; never fails the caller.
function Write-PanelLog([string] $message) {
    try {
        $directory = Split-Path -Parent $script:PanelLogFile
        if (-not (Test-Path -LiteralPath $directory)) { [void](New-Item -ItemType Directory -Path $directory) }
        [IO.File]::AppendAllText($script:PanelLogFile,
            "$([DateTime]::Now.ToString('yyyy-MM-dd HH:mm:ss')) $message`r`n", [Text.UTF8Encoding]::new($false))
    } catch { }
}

function Receive-PanelSpecialAccess($worker) {
    try {
        $results = @($worker.PowerShell.EndInvoke($worker.Handle))
        foreach ($failure in @($worker.PowerShell.Streams.Error)) { Write-PanelLog "special access: $failure" }
        $last = if ($results.Count) { $results[-1] } else { $null }
        if ($last -and $last.PSObject.Properties['ExitCode']) { return $last }
        Write-PanelLog 'special access: the background query returned no result'
    } catch { Write-PanelLog "special access: $($_.Exception.Message)" }
    finally { try { $worker.PowerShell.Dispose() } catch { } }
    return [PSCustomObject]@{ ExitCode = -1; Lines = @() }
}

# Disposes background queries of closed dialogs once they have finished.
function Clear-PanelSpecialAccessWorkers {
    foreach ($worker in @($script:OrphanSpecialAccessWorkers)) {
        if (Test-PanelSpecialAccessDone $worker) {
            $null = Receive-PanelSpecialAccess $worker
            [void]$script:OrphanSpecialAccessWorkers.Remove($worker)
        }
    }
    if ($script:PanelVisitReports -and $script:PanelVisitReports.Count) { Update-PanelVisitReports }
}

function Test-PanelPackageName([string] $pkg) {
    return $pkg -cmatch '^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+$' -and $pkg.Length -le 255
}

function Test-PanelSpecialAccessOp([string] $op) {
    # The ordered table's lookup ignores case; the device's op names do not.
    return @($script:SpecialAccessLabels.Keys) -ccontains $op
}

# A query's answer: the switches (one array, possibly empty: an app requesting none), or
# $null when the device could not answer.
function Read-AppSpecialAccessResult($result) {
    if ($null -eq $result -or $result.ExitCode -ne 0) { return $null }
    return ,@(ConvertFrom-SpecialAccessOutput $result.Lines)
}

# A change's answer: the switch's state afterwards, or $null on failure.
function Read-SetSpecialAccessResult($result, [string] $op) {
    if ($null -eq $result -or $result.ExitCode -ne 0) { return $null }
    $answer = @(ConvertFrom-SpecialAccessOutput $result.Lines) | Where-Object { $_.Op -ceq $op } | Select-Object -First 1
    if (-not $answer) { return $null }
    return [bool]$answer.Allowed
}

# Synchronous forms (diagnostics and tests; the dialog uses the background worker).
function Get-AppSpecialAccess([string] $pkg) {
    if (-not (Test-PanelPackageName $pkg)) { return $null }
    return Read-AppSpecialAccessResult (Invoke-PanelSpecialAccess @($pkg))
}

function Set-AppSpecialAccess([string] $pkg, [string] $op, [bool] $allow) {
    if (-not (Test-PanelPackageName $pkg) -or -not (Test-PanelSpecialAccessOp $op)) { return $null }
    return Read-SetSpecialAccessResult (Invoke-PanelSpecialAccess @($pkg, $op, $(if ($allow) { 'allow' } else { 'deny' }))) $op
}

# A visit belongs to one exact Control Center process, even if Windows reuses its PID.
function Get-PanelVisitIdentity {
    $process = [Diagnostics.Process]::GetCurrentProcess()
    try { return "$($process.Id)", "$($process.StartTime.ToFileTimeUtc())" }
    finally { $process.Dispose() }
}

function Send-PanelSettingsShown([string[]] $tokens) {
    if (-not $tokens.Count) { return }
    $script:PanelVisitReports.Enqueue(@{
        Command = 'settings-shown'; Arguments = @(Get-PanelVisitIdentity) + $tokens
        Mutations = @(); Worker = $null; RetryAfter = [DateTime]::MinValue
    })
}

function Send-PanelSettingsClosed([string[]] $tokens, [object[]] $mutations = @()) {
    if (-not $tokens.Count) { return }
    $script:PanelVisitReports.Enqueue(@{
        Command = 'settings-closed'; Arguments = @(Get-PanelVisitIdentity) + $tokens
        Mutations = $mutations; Worker = $null; RetryAfter = [DateTime]::MinValue
    })
}

# Keep reports ordered and retry transport failures while the host is alive. A
# visit's close is sent only after every settings write from that dialog returned.
function Update-PanelVisitReports {
    $report = $script:PanelVisitReports.Peek()
    if ($report.Worker) {
        if (-not (Test-PanelSpecialAccessDone $report.Worker)) { return }
        $result = Receive-PanelSpecialAccess $report.Worker
        $report.Worker = $null
        # An absent device has no waiting request. An older server has no shown API.
        if ($result.ExitCode -in 0, 3, 4) {
            $null = $script:PanelVisitReports.Dequeue()
            return
        }
        Write-PanelLog "$($report.Command): delivery failed ($($result.ExitCode))"
        $report.RetryAfter = [DateTime]::UtcNow.AddSeconds(1)
    }
    if ($report.RetryAfter -gt [DateTime]::UtcNow) { return }
    foreach ($mutation in $report.Mutations) {
        if (-not (Test-PanelSpecialAccessDone $mutation)) { return }
    }
    $report.Worker = Start-PanelSpecialAccess $report.Arguments $report.Command
}

# The panel window is already closed; give a committed choice its terminal report
# before the host process exits. A failed device cannot leave this process waiting.
function Complete-PanelVisitReportsOnExit([int] $timeoutMs = (2 * $script:SpecialAccessTimeoutMs + 1000)) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($script:PanelVisitReports.Count -gt 0 -and $watch.ElapsedMilliseconds -lt $timeoutMs) {
        try { Update-PanelVisitReports } catch { Write-PanelLog "closing Settings visit: $($_.Exception.Message)" }
        if ($script:PanelVisitReports.Count) { Start-Sleep -Milliseconds 50 }
    }
}

# A request from the system server: one package name. $null for anything else.
function ConvertFrom-PanelRequest([string] $text) {
    if ($null -eq $text) { return $null }
    if ($text -cmatch '^([A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+)$' -and $Matches[1].Length -le 255) {
        return [PSCustomObject]@{ Package = $Matches[1] }
    }
    return $null
}

# Takes the result tokens of the requests waiting for this package's details: each a file
# named by its token, holding the package. Taken files are removed (reported on close).
function Receive-PanelResultTokens([string] $pkg) {
    $tokens = [Collections.Generic.List[string]]::new()
    if (-not $pkg -or -not (Test-Path -LiteralPath $script:PanelResultsDir)) { return $tokens.ToArray() }
    foreach ($file in @(Get-ChildItem -LiteralPath $script:PanelResultsDir -File -ErrorAction SilentlyContinue)) {
        if ($file.Name -cnotmatch '^[0-9a-f]{32}$') { continue }
        try { $owner = [IO.File]::ReadAllText($file.FullName).Trim() } catch { continue }
        if ($owner -cne $pkg) { continue }
        # The device may be reading the file: retry briefly. A token is reported even if its
        # file stays; the device answers each token once and removes its file then.
        foreach ($attempt in 1..5) {
            try { [IO.File]::Delete($file.FullName); break } catch { Start-Sleep -Milliseconds 20 }
        }
        $tokens.Add($file.Name)
    }
    return $tokens.ToArray()
}

# Records the package whose details are shown for a request ($null: none), for
# ControlCenter.publishRequest. Written before the request is taken.
function Set-PanelShowing([string] $pkg) {
    try {
        if ($pkg) { [IO.File]::WriteAllText($script:PanelShowingFile, $pkg) }
        elseif (Test-Path -LiteralPath $script:PanelShowingFile) { [IO.File]::Delete($script:PanelShowingFile) }
    } catch { Write-PanelLog "showing marker: $($_.Exception.Message)" }
}

# Adds result tokens to the details shown, once each.
function Add-PanelShowingTokens([string[]] $tokens) {
    $added = [Collections.Generic.List[string]]::new()
    foreach ($token in $tokens) {
        if ($token -and -not $script:ShowingTokens.Contains($token)) {
            $script:ShowingTokens.Add($token)
            $added.Add($token)
        }
    }
    if ($script:SettingsVisitVisible) { Send-PanelSettingsShown $added.ToArray() }
}

# FromList: the user opened it from the app list, rather than an app from Settings' screens.
function Show-AppInfoDialog([string] $pkg, [switch] $FromList) {
    $item = $appsList.ItemsSource | Where-Object { $_.Package -eq $pkg } | Select-Object -First 1
    if (-not $item) {
        Refresh-Dashboard
        $item = $appsList.ItemsSource | Where-Object { $_.Package -eq $pkg } | Select-Object -First 1
    }
    if (-not $item) { return }
    if ($appsList) {
        if ($appsList.PSObject.Properties['SelectedItem']) { $appsList.SelectedItem = $item }
        if ($appsList.PSObject.Methods['ScrollIntoView']) { $appsList.ScrollIntoView($item) }
    }
    $label = if ($item.Label) { [string]$item.Label } else { $pkg }
    $lines = @()
    $times = Get-AppInstallTimes $pkg
    if ($times) {
        if ($times[0]) { $lines += Get-GesherText 'cp_app_info_installed' @($times[0]) }
        if ($times[1] -and $times[1] -ne $times[0]) { $lines += Get-GesherText 'cp_app_info_updated' @($times[1]) }
    }
    $message = if ($lines.Count -gt 0) { $lines -join "`n" } else { $label }
    # The dialog opens at once; the device's switches load in the background. From the
    # app list, a stopped device is not started for them (the dialog says so).
    $null = Show-ModernConfirmDialog `
        -Title $label `
        -SubTitle $pkg `
        -Message $message `
        -Detail (Get-GesherText $(if ($FromList) { 'cp_app_info_detail_list' } else { 'cp_app_info_detail' })) `
        -CancelText (Get-GesherText 'cp_app_info_close') `
        -IconImage $item.IconImage `
        -SpecialAccessPackage $pkg `
        -CloseOnly
}

# Shows a pending request: brings the panel to the front and, for a package, its details.
# The details shown and the result tokens of the requests they answer: a request for the
# app already shown joins it instead of opening the details again after they close.
$script:ShowingAppInfo = $false
$script:ShowingPackage = $null
$script:ShowingTokens = [Collections.Generic.List[string]]::new()
$script:SettingsVisitVisible = $false
$script:SettingsVisitMutations = @()
$script:PanelVisitReports = [Collections.Generic.Queue[hashtable]]::new()
function Invoke-PanelRequest {
    # Another start signalled: come to the front even while busy or showing a dialog.
    $raise = $false
    if ($script:PanelShowEvent) { try { $raise = $script:PanelShowEvent.WaitOne(0) } catch { } }
    # The dialog runs a nested message loop in which this timer keeps ticking.
    $request = $null
    if ($script:ShowingAppInfo -and $script:ShowingPackage -and -not $script:IsBusy) {
        # Only immutable records for this dialog join it; other packages stay queued.
        # Bound dispatcher work even if tokenless callers keep refilling the queue.
        for ($claimed = 0; $claimed -lt 32; $claimed++) {
            $joined = Receive-PanelRequest $script:ShowingPackage
            if (-not $joined) { break }
            Add-PanelShowingTokens $joined.Tokens
            $raise = $true
        }
        Add-PanelShowingTokens (Receive-PanelResultTokens $script:ShowingPackage)
    } elseif (-not $script:IsBusy -and -not $script:ShowingAppInfo) {
        $request = Receive-PanelRequest
    }
    if ($null -ne $request) { $raise = $true }
    if (-not $raise) { return }
    if ($window.WindowState -eq [System.Windows.WindowState]::Minimized) {
        $window.WindowState = [System.Windows.WindowState]::Normal
    }
    $window.Topmost = $true
    $window.Topmost = $false
    $null = $window.Activate()
    if ($request -and $request.Package) {
        $script:ShowingAppInfo = $true
        $script:ShowingPackage = $request.Package
        $script:ShowingTokens.Clear()
        # Only legacy publishers use this marker; queue ownership never depends on it.
        Set-PanelShowing $request.Package
        Add-PanelShowingTokens $request.Tokens
        Add-PanelShowingTokens (Receive-PanelResultTokens $request.Package)
        try { Show-AppInfoDialog $request.Package }
        finally {
            $tokens = $script:ShowingTokens.ToArray()
            $script:ShowingTokens.Clear()
            $script:ShowingPackage = $null
            $script:ShowingAppInfo = $false
            $script:SettingsVisitVisible = $false
            Set-PanelShowing $null
            if ($tokens.Count) { Send-PanelSettingsClosed $tokens $script:SettingsVisitMutations }
            $script:SettingsVisitMutations = @()
        }
    }
}

# Updates have their own state so a slow check or download does not suspend the
# dashboard or its application controls. Only the installer takes the action guard.
function New-PanelUpdateState($currentVersion, $preferences) {
    return @{
        Closed = $false; Phase = 'Idle'; CurrentVersion = $currentVersion
        Preferences = $preferences; ChangingPreference = $false
        Release = $null; Download = $null; Worker = $null; Installer = $null
        PromptPending = $false; StatusKey = 'cp_update_ready'; StatusValues = @(); Detail = ''
    }
}

function Set-PanelUpdateStatus([string] $key, [object[]] $values = @(), [string] $detail = '') {
    $script:UpdateState.StatusKey = $key
    $script:UpdateState.StatusValues = $values
    $script:UpdateState.Detail = $detail
}

function Update-PanelUpdateView {
    $state = $script:UpdateState
    if ($state.Closed) { return }
    $version = if ($state.CurrentVersion) { $state.CurrentVersion } else { Get-GesherText 'cp_update_unknown' }
    $updateVersionText.Text = Get-GesherText 'cp_update_installed' @($version)
    $updateStatusText.Text = Get-GesherText $state.StatusKey $state.StatusValues
    $updateStatusText.ToolTip = $state.Detail
    $working = $null -ne $state.Worker
    $installing = $state.Phase -in 'Confirming', 'Installing'
    $btnCheckUpdate.IsEnabled = -not ($working -or $installing -or $state.Phase -eq 'Completed')
    $btnInstallUpdate.Visibility = if ($state.Phase -in 'Available', 'Ready') { 'Visible' } else { 'Collapsed' }
    $btnInstallUpdate.IsEnabled = -not $script:IsBusy
    $btnInstallUpdate.Content = if ($state.Download) { Get-GesherText 'cp_update_install' } else { Get-GesherText 'cp_update_download_install' }
    $cancellable = $working -and $state.Worker.Operation -ne 'Install'
    $btnCancelUpdate.Visibility = if ($cancellable -or $state.Phase -eq 'Ready') { 'Visible' } else { 'Collapsed' }
    $btnCancelUpdate.IsEnabled = $state.Phase -ne 'Cancelling'
    $btnUpdateNotes.Visibility = if ($state.Release) { 'Visible' } else { 'Collapsed' }
    $btnReopenPanel.Visibility = if ($state.Phase -eq 'Completed') { 'Visible' } else { 'Collapsed' }
    $updateProgress.Visibility = if ($working -or $state.Phase -eq 'Installing') { 'Visible' } else { 'Collapsed' }
    $automaticUpdateChecks.IsEnabled = -not $installing
    $state.ChangingPreference = $true
    try { $automaticUpdateChecks.IsChecked = [bool]$state.Preferences.AutomaticChecks }
    finally { $state.ChangingPreference = $false }
}

function New-PanelUpdateWorker([string] $operation, $release, [bool] $automatic = $false) {
    $powerShell = [PowerShell]::Create()
    $cancellation = [Threading.CancellationTokenSource]::new()
    try {
        # AddArgument passes data, including paths and release metadata, without
        # interpreting it as PowerShell source. The runspace never sees WPF objects.
        $work = {
            param($helper, $operation, $installDirectory, $destination, $release, $token)
            $ErrorActionPreference = 'Stop'
            . $helper
            if ($operation -eq 'Check') {
                Get-GesherUpdate -InstallDirectory $installDirectory -CancellationToken $token
            } elseif ($operation -eq 'Download') {
                Receive-GesherUpdate -Release $release -DestinationDirectory $destination -CancellationToken $token
            } elseif ($operation -eq 'Install') {
                Start-GesherUpdateInstaller -Download $release -InstallDirectory $installDirectory -CancellationToken $token
            } else {
                throw 'Unknown update operation.'
            }
        }
        $null = $powerShell.AddScript($work.ToString()).AddArgument((Join-Path $here 'gesher-update.ps1')).
            AddArgument($operation).AddArgument($here).AddArgument((Join-Path $script:UpdateDirectory 'downloads')).
            AddArgument($release).AddArgument($cancellation.Token)
        $handle = $powerShell.BeginInvoke()
        return [PSCustomObject]@{
            PowerShell = $powerShell; Handle = $handle; Cancellation = $cancellation
            Operation = $operation; Automatic = $automatic; CancelRequested = $false; Disposed = $false
        }
    } catch {
        $powerShell.Dispose()
        $cancellation.Dispose()
        throw
    }
}

function Dispose-PanelUpdateWorker($worker) {
    try { $worker.PowerShell.Dispose() }
    finally { $worker.Cancellation.Dispose(); $worker.Disposed = $true }
}

function Remove-PanelUpdateDownload($download) {
    if (-not $download) { return }
    try {
        $directory = [IO.Path]::GetFullPath([string]$download.Directory)
        $path = [IO.Path]::GetFullPath([string]$download.Path)
        $staging = [IO.Path]::GetFullPath((Join-Path $script:UpdateDirectory 'downloads'))
        if ([IO.Path]::GetDirectoryName($directory) -ne $staging -or
            [IO.Path]::GetDirectoryName($path) -ne $directory) { return }
        # Only the staged installer and its now-empty unique directory are removed.
        # Never recursively delete the update state, the profile, or another download.
        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
        [IO.Directory]::Delete($directory, $false)
    } catch { }
}

function Start-PanelUpdateCheck([switch] $Automatic) {
    $state = $script:UpdateState
    if ($state.Closed -or $state.Worker -or $state.Installer -or $state.Phase -in 'Confirming', 'Installing', 'Completed') { return }
    $now = [DateTime]::UtcNow
    if ($Automatic -and -not (Test-GesherUpdateCheckDue -Preferences $state.Preferences -Now $now)) { return }
    try {
        $preferences = [PSCustomObject]@{
            AutomaticChecks = [bool]$state.Preferences.AutomaticChecks
            LastCheckUtc = $now.ToString('o')
        }
        # Record attempts before network work, including failed attempts. Manual
        # checks intentionally bypass the daily automatic-check interval.
        Save-GesherUpdatePreferences -StateDirectory $script:UpdateDirectory -Preferences $preferences
        $state.Preferences = $preferences
        Remove-PanelUpdateDownload $state.Download
        $state.Download = $null; $state.Release = $null; $state.PromptPending = $false
        $state.Worker = New-PanelUpdateWorker -operation Check -automatic ([bool]$Automatic)
        $state.Phase = 'Checking'
        Set-PanelUpdateStatus 'cp_update_checking'
    } catch {
        $state.Phase = 'Idle'
        Set-PanelUpdateStatus 'cp_update_check_failed' @() $_.Exception.Message
    }
    Update-PanelUpdateView
}

function Start-PanelUpdateDownload {
    $state = $script:UpdateState
    if ($state.Closed -or $state.Worker -or $state.Installer -or $state.Phase -ne 'Available' -or -not $state.Release) { return }
    try {
        $state.Worker = New-PanelUpdateWorker -operation Download -release $state.Release
        $state.Phase = 'Downloading'
        Set-PanelUpdateStatus 'cp_update_downloading' @($state.Release.Version)
    } catch {
        Set-PanelUpdateStatus 'cp_update_download_failed' @() $_.Exception.Message
    }
    Update-PanelUpdateView
}

function Request-PanelUpdateCancellation {
    $state = $script:UpdateState
    if ($state.Closed -or $state.Installer -or $state.Phase -in 'Confirming', 'Installing') { return }
    $state.PromptPending = $false
    if ($state.Worker) {
        $state.Worker.CancelRequested = $true
        $state.Worker.Cancellation.Cancel()
        $state.Phase = 'Cancelling'
        Set-PanelUpdateStatus 'cp_update_cancelling'
    } else {
        Remove-PanelUpdateDownload $state.Download
        $state.Download = $null
        $state.Phase = if ($state.Release) { 'Available' } else { 'Idle' }
        Set-PanelUpdateStatus 'cp_update_cancelled'
    }
    Update-PanelUpdateView
}

function Complete-PanelUpdateWorker([switch] $Wait) {
    $state = $script:UpdateState
    $worker = $state.Worker
    if (-not $worker -or (-not $Wait -and -not $worker.Handle.IsCompleted)) { return }
    $result = @(); $failure = $null
    try {
        $result = @($worker.PowerShell.EndInvoke($worker.Handle))
        if ($worker.PowerShell.HadErrors) { $failure = [string]$worker.PowerShell.Streams.Error[0] }
    } catch { $failure = $_.Exception.Message }
    finally {
        $cancelled = $worker.CancelRequested -or $worker.Cancellation.IsCancellationRequested
        Dispose-PanelUpdateWorker $worker
        $state.Worker = $null
    }
    if ($worker.Operation -eq 'Install') {
        if (-not $failure -and $result.Count -eq 1 -and $result[0] -is [Diagnostics.Process]) {
            # A process returned after cancellation has already been explicitly
            # approved and started. Never kill setup or delete its running image.
            $state.Installer = $result[0]
            $state.Phase = 'Installing'
            Set-PanelUpdateStatus 'cp_update_installing'
        } else {
            if ($state.Closed -or $cancelled) {
                Remove-PanelUpdateDownload $state.Download
                $state.Download = $null
                $state.Phase = 'Available'
            } else { $state.Phase = 'Ready' }
            Set-PanelInstallationBusy $false
            if ($cancelled) { Set-PanelUpdateStatus 'cp_update_cancelled' }
            else { Set-PanelUpdateStatus 'cp_update_install_failed' @() ([string]$failure) }
        }
        Update-PanelUpdateView
        return
    }
    if ($state.Closed -or $cancelled -or $failure -or $result.Count -ne 1) {
        if ($worker.Operation -eq 'Download') {
            foreach ($download in $result) { Remove-PanelUpdateDownload $download }
        }
        $state.Phase = if ($state.Release) { 'Available' } else { 'Idle' }
        if ($cancelled -or $state.Closed) { Set-PanelUpdateStatus 'cp_update_cancelled' }
        else {
            $key = if ($worker.Operation -eq 'Check') { 'cp_update_check_failed' } else { 'cp_update_download_failed' }
            Set-PanelUpdateStatus $key @() ([string]$failure)
        }
        Update-PanelUpdateView
        return
    }
    if ($worker.Operation -eq 'Check') {
        $check = $result[0]
        $state.CurrentVersion = $check.CurrentVersion
        $state.Release = $check.Release
        switch ($check.Status) {
            'Available' {
                if (-not $state.Release) {
                    $state.Phase = 'Idle'; Set-PanelUpdateStatus 'cp_update_check_failed'
                } else {
                    $state.Phase = 'Available'; Set-PanelUpdateStatus 'cp_update_available' @($state.Release.Version)
                }
            }
            'Current' { $state.Phase = 'Current'; Set-PanelUpdateStatus 'cp_update_current' }
            'NoRelease' { $state.Phase = 'Idle'; Set-PanelUpdateStatus 'cp_update_no_release' }
            'UnknownVersion' { $state.Phase = 'Idle'; Set-PanelUpdateStatus 'cp_update_unknown_version' }
            default { $state.Phase = 'Idle'; $state.Release = $null; Set-PanelUpdateStatus 'cp_update_check_failed' }
        }
    } else {
        $download = $result[0]
        if (-not $download.Path -or -not (Test-Path -LiteralPath $download.Path -PathType Leaf)) {
            Remove-PanelUpdateDownload $download
            $state.Phase = 'Available'; Set-PanelUpdateStatus 'cp_update_download_failed'
        } else {
            $state.Download = $download
            $state.Phase = 'Ready'; $state.PromptPending = $true
            Set-PanelUpdateStatus 'cp_update_download_ready' @($state.Release.Version)
        }
    }
    Update-PanelUpdateView
}

function Set-PanelInstallationBusy([bool] $busy) {
    $btnToggleDevice.IsEnabled = -not $busy
    $servicesCards.IsEnabled = -not $busy
    $applicationsSection.IsEnabled = -not $busy
    $window.AllowDrop = -not $busy
    if ($busy) { Set-Busy $true (Get-GesherText 'cp_update_installing') }
    else { Set-Busy $false }
}

function Show-ModernConfirmDialog {
    param(
        [Parameter(Mandatory)] [string] $Title,
        [Parameter(Mandatory)] [string] $Message,
        [string] $Detail = $null,
        [string] $ConfirmText = (Get-GesherText 'cp_btn_uninstall'),
        [string] $CancelText = (Get-GesherText 'cp_update_cancel'),
        [switch] $IsDanger,
        [switch] $IsWarning,
        $IconImage = $null,
        [string] $SubTitle = $null,
        # Enter closes instead of confirming: for a dialog another app raised (App info),
        # where the user may be typing elsewhere when it takes the focus.
        [switch] $CancelIsDefault,
        # App info: the package whose special access switches load in the background.
        [string] $SpecialAccessPackage = $null,
        # App details can host settings controls without offering an approving action.
        [switch] $CloseOnly
    )

    $badgeBg = if ($IsDanger) { '#FEE4E2' } elseif ($IsWarning) { '#FEF0C7' } else { '#F2F4F7' }
    $badgeFg = if ($IsDanger) { '#D92D20' } elseif ($IsWarning) { '#DC6803' } else { '#344054' }
    $btnStyle = if ($IsDanger) { 'DangerActionButton' } else { 'PrimaryButton' }

    $dialog = New-PanelView 'confirm' @{ PanelFlowDirection = [System.Windows.FlowDirection]$flowDirection; PanelBadgeBackground = [System.Windows.Media.BrushConverter]::new().ConvertFromString($badgeBg) }
    $dialog.Resources['PanelConfirmStyle'] = $dialog.Resources[$btnStyle]
    # Set literal strings after XAML loading: XML attributes normalize newlines
    # and tabs, and XAML can interpret a leading brace as a markup extension.
    $dialog.Title = $Title
    $dialog.FindName('DialogTitle').Text = $Title
    $dialog.FindName('DialogMessage').Text = $Message
    $dialog.FindName('BtnConfirm').Content = $ConfirmText
    $dialog.FindName('BtnCancel').Content = $CancelText
    if ($SubTitle) {
        $dialog.FindName('DialogSubTitle').Text = $SubTitle
        $dialog.FindName('DialogSubTitle').Visibility = 'Visible'
    }
    if ($Detail) {
        $dialog.FindName('DialogDetail').Text = $Detail
        $dialog.FindName('DialogDetail').Visibility = 'Visible'
    }
    $dialog.Owner = $window
    if ($window.Icon) { $dialog.Icon = $window.Icon }

    if ($SpecialAccessPackage) {
        # The switches load and change in background runspaces; a timer of the dialog takes
        # their answers on the UI thread. A switch is disabled while its change is pending.
        $accessState = @{ Query = $null; Pending = @{}; Closed = $false }
        $switchPanel = $dialog.FindName('SwitchPanel')
        $switchStatus = $dialog.FindName('SwitchStatus')
        $switchError = $dialog.FindName('SwitchError')
        $switchList = $dialog.FindName('SwitchList')
        $switchPanel.Visibility = 'Visible'
        $dialog.FindName('SwitchTitle').Text = Get-GesherText 'cp_access_title'
        $switchStatus.Text = Get-GesherText 'cp_access_loading'
        $switchStatus.Visibility = 'Visible'
        $accessTimer = [System.Windows.Threading.DispatcherTimer]::new()
        $accessTimer.Interval = [TimeSpan]::FromMilliseconds(100)
        $accessTimer.Add_Tick({
            if ($accessState.Closed) { return }
            try {
            if ($accessState.Query -and (Test-PanelSpecialAccessDone $accessState.Query)) {
                $switches = Read-AppSpecialAccessResult (Receive-PanelSpecialAccess $accessState.Query)
                $accessState.Query = $null
                $switchStatus.Visibility = 'Collapsed'
                if ($null -eq $switches) {
                    $switchError.Text = Get-GesherText 'cp_access_unavailable'
                    $switchError.Visibility = 'Visible'
                } elseif (@($switches).Count -eq 0) {
                    $switchPanel.Visibility = 'Collapsed'
                } else {
                    foreach ($item in @($switches)) {
                        $box = [System.Windows.Controls.CheckBox]::new()
                        $labelText = Get-GesherText $script:SpecialAccessLabels[[string]$item.Op]
                        $box.Tag = [string]$item.Op
                        $box.IsChecked = [bool]$item.Allowed
                        $box.Margin = [System.Windows.Thickness]::new(0, 2, 0, 2)
                        $box.FontSize = 13
                        if ($item.Fixed) {
                            # The device would answer allowed whatever the switch says.
                            $box.Content = Get-GesherText 'cp_access_fixed' @($labelText)
                            $box.IsEnabled = $false
                        } else {
                            $box.Content = $labelText
                            $box.Add_Click({
                                param($sender, $e)
                                $op = [string]$sender.Tag
                                $wanted = [bool]$sender.IsChecked
                                $switchError.Visibility = 'Collapsed'
                                if ($accessState.Pending.ContainsKey($op)) { $sender.IsChecked = -not $wanted; return }
                                $sender.IsEnabled = $false
                                try {
                                    $arguments = @($SpecialAccessPackage, $op, $(if ($wanted) { 'allow' } else { 'deny' }))
                                    if ($op -ceq 'android:request_install_packages' -and $script:SettingsVisitVisible -and $script:ShowingTokens.Count) {
                                        $arguments += @(Get-PanelVisitIdentity)
                                        $arguments += @($script:ShowingTokens | Select-Object -Last 32)
                                    }
                                    $worker = Start-PanelSpecialAccess $arguments
                                    $accessState.Pending[$op] = @{ Worker = $worker; Box = $sender; Wanted = $wanted }
                                } catch {
                                    $sender.IsEnabled = $true
                                    $sender.IsChecked = -not $wanted
                                    $switchError.Text = Get-GesherText 'cp_access_failed' @([string]$sender.Content)
                                    $switchError.Visibility = 'Visible'
                                }
                            })
                        }
                        [void]$switchList.Children.Add($box)
                    }
                }
            }
            foreach ($op in @($accessState.Pending.Keys)) {
                $change = $accessState.Pending[$op]
                if (-not (Test-PanelSpecialAccessDone $change.Worker)) { continue }
                $accessState.Pending.Remove($op)
                $state = Read-SetSpecialAccessResult (Receive-PanelSpecialAccess $change.Worker) $op
                $change.Box.IsEnabled = $true
                if ($null -eq $state) {
                    $change.Box.IsChecked = -not $change.Wanted
                    $switchError.Text = Get-GesherText 'cp_access_failed' @([string]$change.Box.Content)
                    $switchError.Visibility = 'Visible'
                } else {
                    # What the device answers, not what was asked.
                    $change.Box.IsChecked = [bool]$state
                }
            }
            } catch {
                # A failure here must not leave the dialog loading forever or end the panel.
                Write-PanelLog "special access dialog: $($_.Exception.Message)"
                $switchStatus.Visibility = 'Collapsed'
                $switchError.Text = Get-GesherText 'cp_access_unavailable'
                $switchError.Visibility = 'Visible'
            }
        })
        $dialog.Add_Closed({
            $accessTimer.Stop()
            $accessState.Closed = $true
            # Answers still running are disposed by the live timer once they finish.
            if ($accessState.Query) { $script:OrphanSpecialAccessWorkers.Add($accessState.Query); $accessState.Query = $null }
            $mutations = @($accessState.Pending.Values | ForEach-Object { $_.Worker })
            if ($script:SettingsVisitVisible) { $script:SettingsVisitMutations = $mutations }
            foreach ($mutation in $mutations) { $script:OrphanSpecialAccessWorkers.Add($mutation) }
            $accessState.Pending.Clear()
        })
        # Nothing runs before the dialog is shown: an error before ShowDialog leaves no
        # timer or background query behind.
        $dialog.Add_Loaded({
            if ($accessState.Closed) { return }
            if ($script:ShowingPackage -ceq $SpecialAccessPackage) {
                $script:SettingsVisitVisible = $true
                Send-PanelSettingsShown $script:ShowingTokens.ToArray()
            }
            try { $accessState.Query = Start-PanelSpecialAccess @($SpecialAccessPackage) }
            catch {
                Write-PanelLog "special access: $($_.Exception.Message)"
                $switchStatus.Visibility = 'Collapsed'
                $switchError.Text = Get-GesherText 'cp_access_unavailable'
                $switchError.Visibility = 'Visible'
                return
            }
            $accessTimer.Start()
        })
    }

    $btnConfirm = $dialog.FindName('BtnConfirm')
    $iconContainer = $dialog.FindName('IconContainer')
    $appIconImage = $dialog.FindName('AppIconImage')

    if ($IconImage) {
        $appIconImage.Source = $IconImage
        $appIconImage.Visibility = [System.Windows.Visibility]::Visible
        $iconContainer.Background = [System.Windows.Media.BrushConverter]::new().ConvertFromString('#F2F4F7')
    } else {
        $iconText = [System.Windows.Controls.TextBlock]::new()
        $iconText.Text = if ($IsDanger) { [char]0x2716 } elseif ($IsWarning) { [char]0x26A0 } else { [char]0x2139 }
        $iconText.FontSize = 18
        $iconText.FontWeight = [System.Windows.FontWeights]::Bold
        $iconText.Foreground = [System.Windows.Media.BrushConverter]::new().ConvertFromString($badgeFg)
        $iconText.HorizontalAlignment = [System.Windows.HorizontalAlignment]::Center
        $iconText.VerticalAlignment = [System.Windows.VerticalAlignment]::Center
        $iconContainer.Child = $iconText
    }

    if ($CloseOnly) {
        $btnConfirm.Visibility = 'Collapsed'
        $btnConfirm.IsEnabled = $false
    } else {
        $btnConfirm.Add_Click({
            $dialog.DialogResult = $true
            $dialog.Close()
        })
    }
    if ($CancelIsDefault -or $CloseOnly) {
        $btnCancel = $dialog.FindName('BtnCancel')
        $btnConfirm.IsDefault = $false
        $btnCancel.IsDefault = $true
        $dialog.Add_ContentRendered({ $null = $btnCancel.Focus() })
    }

    $confirmed = $dialog.ShowDialog() -eq $true
    return (-not $CloseOnly -and $confirmed)
}

function Show-UninstallConfirmDialog([string] $pkg, [string] $label = $pkg, $iconImage = $null) {
    $subTitle = if ($label -ne $pkg) { $pkg } else { $null }
    return Show-ModernConfirmDialog `
        -Title (Get-GesherText 'cp_uninstall_title') `
        -SubTitle $subTitle `
        -Message (Get-GesherText 'cp_uninstall_confirm' @($label)) `
        -Detail (Get-GesherText 'cp_uninstall_detail') `
        -ConfirmText (Get-GesherText 'cp_btn_uninstall') `
        -CancelText (Get-GesherText 'cp_update_cancel') `
        -IsDanger `
        -IconImage $iconImage
}

function Show-ModernMessageDialog {
    param(
        [Parameter(Mandatory)] [string] $Title,
        [Parameter(Mandatory)] [string] $Message,
        [bool] $IsError = $false
    )

    $badgeBg = if ($IsError) { '#FEE4E2' } else { '#E0F2FE' }
    $badgeFg = if ($IsError) { '#D92D20' } else { '#0284C7' }

    $dialog = New-PanelView 'message' @{ PanelFlowDirection = [System.Windows.FlowDirection]$flowDirection; PanelBadgeBackground = [System.Windows.Media.BrushConverter]::new().ConvertFromString($badgeBg); PanelBadgeForeground = [System.Windows.Media.BrushConverter]::new().ConvertFromString($badgeFg); PanelMessageSymbol = $(if ($IsError) { '!' } else { 'i' }) }
    $dialog.Title = $Title
    $dialog.FindName('DialogMessage').Text = $Message
    $dialog.FindName('BtnOk').Content = Get-GesherText 'cp_dialog_ok'
    $dialog.Owner = $window
    if ($window.Icon) { $dialog.Icon = $window.Icon }
    $btnOk = $dialog.FindName('BtnOk')
    $btnOk.Add_Click({ $dialog.Close() })
    $dialog.ShowDialog() | Out-Null
}

function Confirm-PanelUpdateInstallation {
    return Show-ModernConfirmDialog `
        -Title (Get-GesherText 'cp_update_title') `
        -Message (Get-GesherText 'cp_update_confirm' @($script:UpdateState.Release.Version)) `
        -ConfirmText (Get-GesherText 'cp_update_install') `
        -CancelText (Get-GesherText 'cp_update_cancel') `
        -IsWarning
}

function Start-PanelUpdateInstallation {
    $state = $script:UpdateState
    if ($state.Closed -or $script:IsBusy -or $state.Worker -or $state.Installer -or
        $state.Phase -ne 'Ready' -or -not $state.Download) { return }
    $state.PromptPending = $false
    $state.Phase = 'Confirming'
    Update-PanelUpdateView
    try {
        if (-not (Confirm-PanelUpdateInstallation) -or $state.Closed) {
            $state.Phase = 'Ready'
            Update-PanelUpdateView
            return
        }
        $state.Phase = 'Installing'
        Set-PanelInstallationBusy $true
        Set-PanelUpdateStatus 'cp_update_installing'
        # Revalidating a large downloaded installer can take time on slow disks.
        # Validation and process creation run off the dispatcher as well.
        $state.Worker = New-PanelUpdateWorker -operation Install -release $state.Download
    } catch {
        $state.Phase = 'Ready'
        Set-PanelInstallationBusy $false
        Set-PanelUpdateStatus 'cp_update_install_failed' @() $_.Exception.Message
    }
    Update-PanelUpdateView
}

function Complete-PanelUpdateInstallation {
    $state = $script:UpdateState
    if (-not $state.Installer -or -not $state.Installer.HasExited) { return }
    $exitCode = $state.Installer.ExitCode
    $state.Installer.Dispose()
    $state.Installer = $null
    try {
        $state.CurrentVersion = Get-GesherInstalledVersion -InstallDirectory $here
        if ($exitCode -eq 0 -and $state.CurrentVersion -ceq $state.Release.Version) {
            $state.Phase = 'Completed'
            Set-PanelUpdateStatus 'cp_update_installed_success' @($state.CurrentVersion)
        } elseif ($exitCode -in 2, 5) {
            $state.Phase = 'Available'
            Set-PanelUpdateStatus 'cp_update_setup_cancelled'
        } elseif ($exitCode -eq 0) {
            $state.Phase = 'Available'
            Set-PanelUpdateStatus 'cp_update_unverified'
        } else {
            $state.Phase = 'Available'
            Set-PanelUpdateStatus 'cp_update_install_failed' @() (Get-GesherText 'cp_update_exit_code' @($exitCode))
        }
    } catch {
        $state.Phase = 'Available'
        Set-PanelUpdateStatus 'cp_update_unverified' @() $_.Exception.Message
    } finally {
        Remove-PanelUpdateDownload $state.Download
        $state.Download = $null
        Set-PanelInstallationBusy $false
        try { Refresh-Dashboard } catch { }
    }
    Update-PanelUpdateView
}

function Invoke-PanelUpdateTick {
    $state = $script:UpdateState
    if ($state.Closed) { return }
    Complete-PanelUpdateWorker
    Complete-PanelUpdateInstallation
    # File pickers and existing confirmation dialogs run nested dispatcher loops
    # before some dashboard actions take IsBusy. Their inactive owner must not
    # start setup; it may resume an application operation after the dialog closes.
    if ($state.PromptPending -and -not $script:IsBusy -and $window.IsActive) { Start-PanelUpdateInstallation }
}

function Set-PanelAutomaticUpdateChecks([bool] $enabled) {
    $state = $script:UpdateState
    if ($state.Closed -or $state.ChangingPreference) { return }
    try {
        $preferences = [PSCustomObject]@{
            AutomaticChecks = $enabled; LastCheckUtc = $state.Preferences.LastCheckUtc
        }
        Save-GesherUpdatePreferences -StateDirectory $script:UpdateDirectory -Preferences $preferences
        $state.Preferences = $preferences
        if (-not $enabled -and $state.Worker -and $state.Worker.Automatic) { Request-PanelUpdateCancellation }
        elseif ($enabled) { Start-PanelUpdateCheck -Automatic }
    } catch { Set-PanelUpdateStatus 'cp_update_preferences_failed' @() $_.Exception.Message }
    Update-PanelUpdateView
}

function Close-PanelUpdates {
    $state = $script:UpdateState
    if ($state.Closed) { return }
    $state.Closed = $true
    $state.PromptPending = $false
    if ($state.Worker) {
        $state.Worker.CancelRequested = $true
        $state.Worker.Cancellation.Cancel()
    }
    if (-not $state.Installer -and -not $state.Worker) {
        Remove-PanelUpdateDownload $state.Download
        $state.Download = $null
    }
}

$script:UpdateState = New-PanelUpdateState (Get-GesherInstalledVersion -InstallDirectory $here) `
    (Get-GesherUpdatePreferences -StateDirectory $script:UpdateDirectory)
Update-PanelUpdateView
$btnCheckUpdate.Add_Click({ Start-PanelUpdateCheck })
$btnInstallUpdate.Add_Click({
    if ($script:UpdateState.Download) { Start-PanelUpdateInstallation }
    else { Start-PanelUpdateDownload }
})
$btnCancelUpdate.Add_Click({ Request-PanelUpdateCancellation })
$automaticUpdateChecks.Add_Checked({ Set-PanelAutomaticUpdateChecks $true })
$automaticUpdateChecks.Add_Unchecked({ Set-PanelAutomaticUpdateChecks $false })

function Update-CameraView {
    if ($null -eq $cameraImagePickerFallback) { return }
    $script:UpdatingCameraView = $true
    try { $cameraImagePickerFallback.IsChecked = [bool](Get-CameraImagePickerEnabled) }
    finally { $script:UpdatingCameraView = $false }
}

function Set-PanelCameraImagePickerFallback([bool] $enabled) {
    if ($script:UpdatingCameraView) { return }
    try {
        Set-CameraImagePickerEnabled $enabled
        $statusMessage.Text = Get-GesherText 'cp_camera_image_picker_saved'
    } catch {
        Show-ModernMessageDialog -Title (Get-GesherText 'cp_title') -Message (Get-GesherText 'cp_camera_image_picker_failed' @($_.Exception.Message)) -IsError $true
        Update-CameraView
    }
}

$cameraImagePickerFallback.Add_Checked({ Set-PanelCameraImagePickerFallback $true })
$cameraImagePickerFallback.Add_Unchecked({ Set-PanelCameraImagePickerFallback $false })
$btnUpdateNotes.Add_Click({
    try {
        if ($script:UpdateState.Release) { Start-Process -FilePath $script:UpdateState.Release.Url }
    } catch { Set-PanelUpdateStatus 'cp_update_notes_failed' @() $_.Exception.Message; Update-PanelUpdateView }
})
$btnReopenPanel.Add_Click({
    try {
        # This panel is still open as the new one starts: let it through (one panel).
        $env:GESHER_PANEL_REOPEN = '1'
        try {
            Start-Process -FilePath (Join-Path $here 'gesher-launch.exe') -ArgumentList 'control-panel' -WindowStyle Hidden
        } finally { Remove-Item Env:GESHER_PANEL_REOPEN -ErrorAction SilentlyContinue }
        $window.Close()
    } catch { Set-PanelUpdateStatus 'cp_update_reopen_failed' @() $_.Exception.Message; Update-PanelUpdateView }
})
$window.Add_SizeChanged({ Update-PanelWindowLayout })
$window.Add_ContentRendered({
    Update-PanelWindowLayout
    Start-PanelUpdateCheck -Automatic
    try { Invoke-PanelRequest } catch { }
})

# Toggle device handler
$btnToggleDevice.Add_Click({
    if ($script:IsBusy) { return }
    $command = if (Get-DeviceStatus) { 'shutdown' } else { 'start' }
    [void](Start-PanelAction 'Device' @{ Command = $command })
})

# microG Handlers
$btnEnableMicrog.Add_Click({
    [void](Start-PanelAction 'Microg' @{ Enable = $true } (Get-GesherText 'cp_staging'))
})

$btnGoogleLogin.Add_Click({
    if ($script:IsBusy) { return }
    Start-Process -FilePath (Join-Path $here 'gesher-launch.exe') -ArgumentList 'launch', 'com.google.android.gms' -WindowStyle Hidden
})

$btnMicrogSettings.Add_Click({
    if ($script:IsBusy) { return }
    Start-Process -FilePath (Join-Path $here 'gesher-launch.exe') -ArgumentList 'launch', 'com.google.android.gms' -WindowStyle Hidden
})

$btnDisableMicrog.Add_Click({
    [void](Start-PanelAction 'Microg' @{ Enable = $false })
})

. (Join-Path $here 'gesher-panel-actions.ps1')

function Start-PanelAction([string] $operation, $request, [string] $message = '') {
    $state = $script:ActionState
    if ($state.Closed -or $state.Worker -or $state.Completing -or $script:IsBusy -or
        $script:UpdateState.Phase -in 'Confirming', 'Installing') { return $false }
    Set-Busy $true $message
    try {
        $state.Worker = New-PanelActionWorker $operation $request
        return $true
    } catch {
        try { Show-ModernMessageDialog -Title (Get-GesherText 'cp_title') -Message $_.Exception.Message -IsError $true }
        finally { Complete-DashboardAction }
        return $false
    }
}

function Complete-PanelAction {
    $state = $script:ActionState
    $worker = $state.Worker
    if (-not $worker -or -not $worker.Handle.IsCompleted -or $state.Completing) { return }
    $state.Completing = $true
    $failures = [Collections.Generic.List[string]]::new()
    try {
        try {
            $results = @($worker.PowerShell.EndInvoke($worker.Handle))
            foreach ($result in $results) {
                foreach ($failure in $result.Errors) {
                    if ($failure) { $failures.Add([string]$failure) }
                }
            }
            # A terminating runspace error can complete without producing the
            # structured result; don't acknowledge an unobserved action as success.
            if ($results.Count -ne 1) { throw 'Dashboard operation did not return a result.' }
        } catch { $failures.Add($_.Exception.Message) }
        finally {
            try { $worker.PowerShell.Dispose() }
            catch { $failures.Add($_.Exception.Message) }
            finally { $worker.Disposed = $true; $state.Worker = $null }
        }
        if ($failures.Count) {
            Show-ModernMessageDialog -Title (Get-GesherText 'cp_title') -Message ($failures -join "`n`n") -IsError $true
        }
    } finally {
        try { Complete-DashboardAction } finally { $state.Completing = $false }
    }
}

function Test-PanelActionClose {
    # Completion releases ownership even if the dashboard's subsequent refresh
    # needs a retry. A transient read failure must not trap an otherwise idle UI.
    try { Complete-PanelAction } catch { }
    if ($script:ActionState.Worker -or $script:ActionState.Completing) {
        $statusMessage.Text = Get-GesherText 'cp_action_finish'
        return $false
    }
    return $true
}

# Store Handlers
$btnInstallStore.Add_Click({
    [void](Start-PanelAction 'Store' @{} (Get-GesherText 'cp_downloading' @('Aurora Store')))
})

$btnOpenStore.Add_Click({
    if ($script:IsBusy) { return }
    Start-Process -FilePath (Join-Path $here 'gesher-launch.exe') -ArgumentList 'launch', 'com.aurora.store' -WindowStyle Hidden
})

# Install APK Button Handler
$btnInstallApk.Add_Click({
    if ($script:IsBusy) { return }
    $ofd = [Microsoft.Win32.OpenFileDialog]::new()
    $ofd.Filter = "Android Packages (*.apk;*.apks;*.xapk;*.apkm;*.apk+)|*.apk;*.apks;*.xapk;*.apkm;*.apk+|All Files (*.*)|*.*"
    if ($ofd.ShowDialog() -eq $true) {
        $file = $ofd.FileName
        $fileName = [System.IO.Path]::GetFileName($file)
        [void](Start-PanelAction 'Install' @{ Files = @($file) } (Get-GesherText 'cp_installing' @($fileName)))
    }
})

# Drag & Drop Handler
$window.Add_Drop({
    param($sender, $e)
    if ($script:IsBusy) { return }
    if ($e.Data.GetDataPresent([System.Windows.DataFormats]::FileDrop)) {
        $files = $e.Data.GetData([System.Windows.DataFormats]::FileDrop)
        $packages = @($files | Where-Object { [IO.Path]::GetExtension($_).ToLowerInvariant() -in '.apk', '.apks', '.xapk', '.apkm', '.apk+' })
        if ($packages.Count) {
            [void](Start-PanelAction 'Install' @{ Files = $packages } (Get-GesherText 'cp_installing' @([IO.Path]::GetFileName($packages[0]))))
        }
    }
})

# Per-app Launch and Uninstall handlers (invoked via event binding or code behind)
$script:OnAppLaunch = {
    param($pkg)
    if ($script:IsBusy) { return }
    Start-Process -FilePath (Join-Path $here 'gesher-launch.exe') -ArgumentList 'launch', $pkg -WindowStyle Hidden
}

# The app's details (uninstall, special access), as an app's Settings screen opens them.
$script:OnAppDetails = {
    param($pkg)
    if ($script:IsBusy -or $script:ShowingAppInfo) { return }
    $script:ShowingAppInfo = $true
    try { Show-AppInfoDialog $pkg -FromList } finally { $script:ShowingAppInfo = $false }
}

$script:OnAppUninstall = {
    param($pkg)
    if ($script:IsBusy) { return }
    $item = $appsList.ItemsSource | Where-Object { $_.Package -eq $pkg } | Select-Object -First 1
    $label = if ($item -and $item.Label) { $item.Label } else { $pkg }
    $iconImage = if ($item) { $item.IconImage } else { $null }
    Set-Busy $true
    try { $confirmed = Show-UninstallConfirmDialog -pkg $pkg -label $label -iconImage $iconImage }
    finally { Set-Busy $false }
    if ($confirmed) {
        [void](Start-PanelAction 'Uninstall' @{ Package = [string]$pkg; Label = [string]$label })
    }
}

# The proxy of every app ($pkg empty) or of one app (proxy.<package>, over every app's
# setting); app processes started from now on use it. True when it was saved.
function Show-ProxyDialog([string] $pkg = '', [string] $label = $pkg) {
    [xml]$dialogXaml = @"
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Width="460" SizeToContent="Height"
        WindowStartupLocation="CenterOwner"
        ResizeMode="NoResize"
        ShowInTaskbar="False"
        FlowDirection="$flowDirection"
        FontFamily="Segoe UI, Tahoma, Arial"
        Background="#FFFFFF">
    <Window.Resources>
        <Style TargetType="Button">
            <Setter Property="Padding" Value="16,8"/>
            <Setter Property="FontSize" Value="13"/>
            <Setter Property="FontWeight" Value="SemiBold"/>
            <Setter Property="Cursor" Value="Hand"/>
            <Setter Property="BorderThickness" Value="1"/>
            <Setter Property="BorderBrush" Value="#D0D5DD"/>
            <Setter Property="Background" Value="#FFFFFF"/>
            <Setter Property="Foreground" Value="#344054"/>
            <Setter Property="Template">
                <Setter.Value>
                    <ControlTemplate TargetType="Button">
                        <Border Background="{TemplateBinding Background}"
                                BorderBrush="{TemplateBinding BorderBrush}"
                                BorderThickness="{TemplateBinding BorderThickness}"
                                CornerRadius="6"
                                Padding="{TemplateBinding Padding}">
                            <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
                        </Border>
                    </ControlTemplate>
                </Setter.Value>
            </Setter>
        </Style>
    </Window.Resources>
    <Border Padding="22">
        <StackPanel>
            <TextBlock Name="DialogHeading" FontSize="15" FontWeight="Bold" Foreground="#101828" TextWrapping="Wrap"/>
            <TextBlock Name="DialogPackage" FontSize="12" Foreground="#667085" Margin="0,2,0,0"/>
            <TextBlock Name="DialogDetail" FontSize="12" Foreground="#475467" TextWrapping="Wrap" Margin="0,10,0,14"/>
            <StackPanel Orientation="Horizontal">
                <ComboBox Name="DialogProxyMode" Width="190" Padding="8,7" VerticalContentAlignment="Center" Margin="0,0,8,0"/>
                <TextBox Name="DialogProxyAddress" Width="170" Padding="6,7" VerticalContentAlignment="Center" FlowDirection="LeftToRight"/>
            </StackPanel>
            <TextBlock Name="DialogError" FontSize="12" Foreground="#B42318" TextWrapping="Wrap" Margin="0,8,0,0" Visibility="Collapsed"/>
            <StackPanel Orientation="Horizontal" HorizontalAlignment="Right" Margin="0,20,0,0">
                <Button Name="BtnCancel" IsCancel="True" MinWidth="85" Margin="0,0,8,0"/>
                <Button Name="BtnSave" IsDefault="True" MinWidth="85" Background="#1570EF" BorderBrush="#1570EF" Foreground="#FFFFFF"/>
            </StackPanel>
        </StackPanel>
    </Border>
</Window>
"@
    $dialog = [System.Windows.Markup.XamlReader]::Load([System.Xml.XmlNodeReader]::new($dialogXaml))
    $heading = if ($pkg) { Get-GesherText 'cp_proxy_app_title' @($label) } else { Get-GesherText 'cp_btn_proxy_all' }
    $dialog.Title = $heading
    $dialog.FindName('DialogHeading').Text = $heading
    $packageText = $dialog.FindName('DialogPackage')
    if ($pkg -and $label -ne $pkg) { $packageText.Text = $pkg } else { $packageText.Visibility = [System.Windows.Visibility]::Collapsed }
    $dialog.FindName('DialogDetail').Text = if ($pkg) { Get-GesherText 'cp_proxy_app_desc' } else { Get-GesherText 'cp_proxy_desc' }
    $dialog.FindName('BtnCancel').Content = Get-GesherText 'cp_update_cancel'
    $dialog.FindName('BtnSave').Content = Get-GesherText 'cp_proxy_save'
    $mode = $dialog.FindName('DialogProxyMode')
    $address = $dialog.FindName('DialogProxyAddress')
    $address.ToolTip = Get-GesherText 'cp_proxy_address_hint'
    $current = $null
    [void](Get-ProxySettings).TryGetValue($(if ($pkg) { "proxy.$pkg" } else { 'proxy' }), [ref] $current)
    Initialize-ProxyEditor $mode $address $current -WithInherit:([bool]$pkg)
    $dialog.Owner = $window
    if ($window.Icon) { $dialog.Icon = $window.Icon }
    $state = @{ Saved = $false }
    $dialog.FindName('BtnSave').Add_Click({
        $choice = Read-ProxyEditor $mode $address
        $errorText = $dialog.FindName('DialogError')
        if (-not $choice.Valid) {
            $errorText.Text = Get-GesherText 'cp_proxy_invalid' @($choice.Text)
            $errorText.Visibility = [System.Windows.Visibility]::Visible
            [void]$address.Focus()
            return
        }
        try {
            Set-ProxySetting $pkg $choice.Value
            $state.Saved = $true
            $dialog.Close()
        } catch {
            $errorText.Text = Get-GesherText 'cp_proxy_save_failed' @($_.Exception.Message)
            $errorText.Visibility = [System.Windows.Visibility]::Visible
        }
    })
    $dialog.ShowDialog() | Out-Null
    return $state.Saved
}

$script:OnAppProxy = {
    param($pkg)
    if ($script:IsBusy) { return }
    $item = $appsList.ItemsSource | Where-Object { $_.Package -eq $pkg } | Select-Object -First 1
    $label = if ($item -and $item.Label) { $item.Label } else { $pkg }
    if (Show-ProxyDialog -pkg $pkg -label $label) {
        Refresh-Dashboard
        $statusMessage.Text = Get-GesherText 'cp_proxy_saved'
    }
}

$btnProxyAll.Add_Click({
    if ($script:IsBusy) { return }
    if (Show-ProxyDialog) { $statusMessage.Text = Get-GesherText 'cp_proxy_saved' }
})

# The profile's logs folder (gesher-app.ps1 writes it; a portable install keeps its profile
# beside the runtime), opened in File Explorer. Created first so a new install opens it too.
function Open-PanelLogsFolder([string] $logs) {
    [void][IO.Directory]::CreateDirectory($logs)
    Start-Process -FilePath (Join-Path $env:WINDIR 'explorer.exe') -ArgumentList ('"' + $logs + '"')
}

$btnOpenLogs = $window.FindName('BtnOpenLogs')
$btnOpenLogs.ToolTip = Join-Path $root 'logs'
$btnOpenLogs.Add_Click({
    try { Open-PanelLogsFolder (Join-Path $root 'logs') }
    catch {
        Show-ModernMessageDialog -Title (Get-GesherText 'cp_title') -Message (Get-GesherText 'cp_open_logs_failed' @($_.Exception.Message)) -IsError $true
    }
})

# Attach click events through visual tree dispatcher
$appsList.AddHandler([System.Windows.Controls.Button]::ClickEvent, [System.Windows.RoutedEventHandler]{
    param($sender, $e)
    $box = $e.OriginalSource -as [System.Windows.Controls.CheckBox]
    if ($box -and $box.Tag) {
        try { Set-AppSeparateWindows ([string]$box.Tag) ([bool]$box.IsChecked) } catch { }
        return
    }
    $btn = $e.OriginalSource -as [System.Windows.Controls.Button]
    if ($btn -and $btn.Tag) {
        $pkg = [string]$btn.Tag
        if ($btn.Content -eq (Get-GesherText 'cp_btn_launch')) {
            & $script:OnAppLaunch $pkg
        } elseif ($btn.Content -eq (Get-GesherText 'cp_btn_app_info')) {
            & $script:OnAppDetails $pkg
        } elseif ($btn.Content -eq (Get-GesherText 'cp_btn_uninstall')) {
            & $script:OnAppUninstall $pkg
        } elseif ($btn.Content -eq (Get-GesherText 'cp_btn_proxy')) {
            & $script:OnAppProxy $pkg
        }
    }
})

# Live refresh: reflect devices started/stopped, apps installed and microG toggled outside the panel.
# Handler failures (e.g. a file mid-rewrite) are dropped; the next tick or activation retries.
$script:LiveTimer = [System.Windows.Threading.DispatcherTimer]::new()
$script:LiveTimer.Interval = [TimeSpan]::FromMilliseconds($script:LiveRefreshIntervalMs)
$script:LiveTimer.Add_Tick({ Invoke-PanelHeartbeat })
Initialize-PanelMetadata
$window.Add_Closing({
    param($sender, $event)
    if (-not (Test-PanelActionClose)) { $event.Cancel = $true; return }
    # Finish or cancel the separate setup wizard before closing its progress view.
    # Checking and downloading can always be cancelled by closing this window.
    Complete-PanelUpdateInstallation
    if ($script:UpdateState.Installer -or $script:UpdateState.Phase -eq 'Installing') {
        $event.Cancel = $true
        Set-PanelUpdateStatus 'cp_update_finish_setup'
        Update-PanelUpdateView
    }
})
$window.Add_Closed({
    $script:ActionState.Closed = $true
    Close-PanelUpdates
    $script:LiveTimer.Stop()
    # Free the single-panel name now, not after the update worker drains: a start right
    # after this window closed opens a new panel instead of signalling this one.
    try { $script:PanelShowEvent.Dispose() } catch { }
    try { $script:PanelInstance.Dispose() } catch { }
    $script:PanelShowEvent = $null
})

# Initial load
Refresh-Dashboard
$script:LiveTimer.Start()
try { $window.ShowDialog() | Out-Null }
finally {
    $script:LiveTimer.Stop()
    Complete-PanelVisitReportsOnExit
    Close-PanelMetadata
    Close-PanelUpdates
    # The window is already closed. Drain the cooperatively cancelled, bounded
    # request here so the worker can delete partial files before its runspace ends.
    # EndInvoke is never allowed to wait on the live WPF dispatcher.
    Complete-PanelUpdateWorker -Wait
    if ($script:UpdateState.Installer) { $script:UpdateState.Installer.Dispose() }
}
