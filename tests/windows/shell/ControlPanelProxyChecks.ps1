# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# The control panel's proxy settings (every app, one app) run the production functions on a
# scratch proxy.properties: ProxySetting.parse's validation, the java.util.Properties
# format ProxySetting.save writes and reads, ProxySetting.save's removal rules, and the
# mode/address editor on real WPF controls.
param(
    [string] $PanelPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-control-panel.ps1')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
. (Join-Path $repo 'tools/package/gesher-ui.ps1')
$script:GesherUiLanguage = 'en'
$source = [IO.File]::ReadAllText([IO.Path]::GetFullPath($PanelPath))
$tokens = $null; $parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseInput($source, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
foreach ($name in 'ConvertTo-ProxyValue', 'Read-ProxyProperties', 'ConvertFrom-PropertiesEscapes',
        'ConvertTo-PropertiesValue', 'Get-ProxySettings', 'Set-ProxySetting', 'Get-ProxyDisplayText',
        'Initialize-ProxyEditor', 'Read-ProxyEditor', 'Get-FileStamp', 'Get-StateStamp', 'Get-AppFilesStamp',
        'Update-AppsView', 'Test-PanelMetadataVisible', 'Request-PanelMetadataRefresh', 'Update-PanelMetadataWatch',
        'Refresh-Dashboard', 'Invoke-LiveRefreshTick') {
    $definitions = @($ast.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $false))
    if ($definitions.Count -ne 1) { throw "Expected one production function: $name" }
    . ([scriptblock]::Create($definitions[0].Extent.Text))
}

$script:Checks = 0
function Assert-Proxy([bool] $value, [string] $message) {
    if (-not $value) { throw $message }
    $script:Checks++
}

# ProxySetting.parse's values.
foreach ($case in @(
        @('127.0.0.1:8080', '127.0.0.1:8080'), @(' http://proxy.example.com:3128/ ', 'proxy.example.com:3128'),
        @('[::1]:8888', '[::1]:8888'), @('DIRECT', 'direct'), @('none', 'direct'), @('off', 'direct'),
        @('windows', 'windows'), @('Default', 'windows'),
        # Integer.parseInt: a '+' sign, leading zeros and decimal digits of any script.
        @('proxy.example.com:+80', 'proxy.example.com:+80'), @('host:0080', 'host:0080'),
        @("host:$([char]0xFF18)$([char]0xFF10)", "host:$([char]0xFF18)$([char]0xFF10)"),
        @("$([char]1)host:80$([char]31)", 'host:80'),
        @('proxy.example.com', $null), @(':8080', $null), @('host:', $null), @('host:0', $null),
        @('host:65536', $null), @('host:-80', $null), @('host:+', $null), @('host:++80', $null),
        @('host:99999999999999999999', $null), @('host:8 0', $null), @("ho`nst:80", $null),
        @("host`n:80", $null), @('ho st:80', $null), @('https://host:80', $null), @('', $null))) {
    $actual = ConvertTo-ProxyValue $case[0]
    Assert-Proxy ($actual -ceq $case[1]) "ConvertTo-ProxyValue '$($case[0])' gave '$actual', expected '$($case[1])'"
}

$scratch = Join-Path ([IO.Path]::GetTempPath()) ('gesher-panel-proxy-' + [Guid]::NewGuid().ToString('N'))
$script:ProxyFile = Join-Path $scratch 'data/system/proxy.properties'
try {
    Assert-Proxy ((Get-ProxySettings).Count -eq 0) 'No file must mean no settings'

    # A file as java.util.Properties.store writes it (escaped ':'), with a comment and another key.
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $script:ProxyFile))
    [IO.File]::WriteAllLines($script:ProxyFile, [string[]]@(
        '#Gesher''s proxy: gesher-app proxy [PACKAGE] host:port|direct|windows',
        '#Mon Nov 23 10:00:00 IST 2026',
        'proxy=10.0.0.1\:3128',
        'proxy.com.example.app=direct',
        'proxy.com.example.other=bad value',
        'unrelated=1'), [Text.UTF8Encoding]::new($false))
    $settings = Get-ProxySettings
    Assert-Proxy ($settings['proxy'] -ceq '10.0.0.1:3128') 'Java-escaped value was not read'
    Assert-Proxy ($settings['proxy.com.example.app'] -ceq 'direct') 'Package value was not read'
    Assert-Proxy (-not $settings.ContainsKey('proxy.com.example.other')) 'An invalid value must be ignored, as ProxySetting.parse does'
    Assert-Proxy ($settings.Count -eq 2) 'Only proxy keys are settings'

    # Every app: replace the value; other lines are kept.
    Set-ProxySetting '' '192.168.1.5:8080'
    $text = [IO.File]::ReadAllText($script:ProxyFile)
    Assert-Proxy ($text.Contains('proxy=192.168.1.5\:8080')) 'Every app''s value is not saved in Properties form'
    Assert-Proxy (-not $text.Contains('10.0.0.1')) 'The previous value of every app was kept'
    Assert-Proxy ($text.Contains('proxy.com.example.app=direct') -and $text.Contains('unrelated=1')) 'Other lines were lost'
    Assert-Proxy ((Get-ProxySettings)['proxy'] -ceq '192.168.1.5:8080') 'Every app''s value does not round-trip'

    # One app: its own setting, 'windows' kept for a package, $null removes it.
    Set-ProxySetting 'com.example.app' '[::1]:8888'
    Assert-Proxy ((Get-ProxySettings)['proxy.com.example.app'] -ceq '[::1]:8888') 'Package proxy does not round-trip'
    Assert-Proxy ([IO.File]::ReadAllText($script:ProxyFile).Contains('proxy.com.example.app=[\:\:1]\:8888')) 'IPv6 value is not escaped as Properties does'
    Set-ProxySetting 'com.example.app' 'windows'
    Assert-Proxy ((Get-ProxySettings)['proxy.com.example.app'] -ceq 'windows') 'Windows'' settings for a package must be kept'
    Set-ProxySetting 'com.example.app' $null
    Assert-Proxy (-not (Get-ProxySettings).ContainsKey('proxy.com.example.app')) 'Removing a package setting left it behind'
    Assert-Proxy ((Get-ProxySettings)['proxy'] -ceq '192.168.1.5:8080') 'Removing a package setting changed every app''s'

    # 'windows' for every app removes the key (ProxySetting.save).
    Set-ProxySetting '' 'windows'
    Assert-Proxy (-not (Get-ProxySettings).ContainsKey('proxy')) 'Windows'' settings for every app must remove the key'
    Set-ProxySetting '' 'direct'
    Assert-Proxy ((Get-ProxySettings)['proxy'] -ceq 'direct') 'Direct for every app was not saved'

    $threw = $false
    try { Set-ProxySetting 'com.example.app' 'nonsense' } catch { $threw = $true }
    Assert-Proxy $threw 'An invalid value must not be saved'
    $threw = $false
    try { Set-ProxySetting 'bad package' 'direct' } catch { $threw = $true }
    Assert-Proxy $threw 'An invalid package must not be saved'

    # A new file starts with ProxySetting's comment.
    Remove-Item -LiteralPath $script:ProxyFile
    Set-ProxySetting 'com.example.new' '127.0.0.1:9000'
    Assert-Proxy ((Get-Content -LiteralPath $script:ProxyFile)[0].StartsWith('#')) 'A new file has no comment'
    Assert-Proxy ((Get-ProxySettings)['proxy.com.example.new'] -ceq '127.0.0.1:9000') 'Package proxy in a new file does not round-trip'

    # Lines as java.util.Properties.load reads them: separators, continuations, escapes,
    # the last line of a key winning (also when it is invalid), ISO 8859-1.
    [IO.File]::WriteAllLines($script:ProxyFile, [string[]]@(
        '! another comment',
        '  proxy.com.a = 10.0.0.2\:81',
        'proxy.com.b:direct',
        "proxy.com.c`t1.2.3.4\:82",
        'proxy.com.d=10.0.0.3\:\',
        '    83',
        'proxy.com.e=10.0.0.4\u003a84',
        'proxy.com.f=10.0.0.5\:85',
        'proxy.com.f=nonsense',
        'proxy.com.g=host\:\uff18\uff10',
        'proxy\u002ecom.h=direct',
        'unrelated=a\',
        '  proxy.com.z=1.1.1.1\:1'), [Text.Encoding]::GetEncoding(28591))
    $settings = Get-ProxySettings
    Assert-Proxy ($settings['proxy.com.a'] -ceq '10.0.0.2:81') 'Spaces around = were not read'
    Assert-Proxy ($settings['proxy.com.b'] -ceq 'direct') 'The : separator was not read'
    Assert-Proxy ($settings['proxy.com.c'] -ceq '1.2.3.4:82') 'The whitespace separator was not read'
    Assert-Proxy ($settings['proxy.com.d'] -ceq '10.0.0.3:83') 'A continued line was not read'
    Assert-Proxy ($settings['proxy.com.e'] -ceq '10.0.0.4:84') 'A \u escape was not read'
    Assert-Proxy (-not $settings.ContainsKey('proxy.com.f')) 'An earlier line of a key won over its last one'
    Assert-Proxy ($settings['proxy.com.g'] -ceq "host:$([char]0xFF18)$([char]0xFF10)") 'A non-ASCII escaped port was not read'
    Assert-Proxy ($settings['proxy.com.h'] -ceq 'direct') 'An escaped key was not read'
    Assert-Proxy (-not $settings.ContainsKey('proxy.com.z')) 'A continuation line was read as a key'
    Set-ProxySetting 'com.d' 'direct'
    Set-ProxySetting 'com.g' "host:$([char]0xFF19)0"
    $lines = [IO.File]::ReadAllLines($script:ProxyFile, [Text.Encoding]::GetEncoding(28591))
    Assert-Proxy (@($lines | Where-Object { $_ -like '*83*' }).Count -eq 0) 'A continued line of a replaced key was left behind'
    Assert-Proxy ($lines -ccontains 'proxy.com.g=host\:\uFF190') 'A non-ASCII value is not written as Properties.store does'
    Assert-Proxy ($lines -ccontains 'unrelated=a\' -and $lines -ccontains '  proxy.com.z=1.1.1.1\:1') 'Another key''s continuation was changed'
    $settings = Get-ProxySettings
    Assert-Proxy ($settings['proxy.com.d'] -ceq 'direct' -and $settings['proxy.com.a'] -ceq '10.0.0.2:81' -and
        $settings['proxy.com.g'] -ceq "host:$([char]0xFF19)0") 'Saving changed or lost settings'
    Assert-Proxy ((ConvertTo-PropertiesValue ' a=b:c#d!e\f') -ceq '\ a\=b\:c\#d\!e\\f') 'Properties.store escapes are not written'

    # A UTF-8 byte order mark is part of the first key for Properties.load(InputStream),
    # so that line is not a setting; saving keeps the bytes of every other line.
    $savedFile = [IO.File]::ReadAllBytes($script:ProxyFile)
    function Write-ProxyLines([string[]] $lines) {
        [IO.File]::WriteAllLines($script:ProxyFile, $lines, [Text.Encoding]::GetEncoding(28591))
    }
    $bom = [byte[]](0xEF, 0xBB, 0xBF)
    [IO.File]::WriteAllBytes($script:ProxyFile, $bom + [Text.Encoding]::GetEncoding(28591).GetBytes(
        "proxy=direct`n#caf$([char]0xE9) comment`nproxy.com.k=direct`n"))
    $settings = Get-ProxySettings
    Assert-Proxy (-not $settings.ContainsKey('proxy')) 'A key after a byte order mark was read as every app''s setting'
    Assert-Proxy ($settings['proxy.com.k'] -ceq 'direct') 'A setting after the byte order mark line was not read'
    Set-ProxySetting 'com.k' $null
    $bytes = [IO.File]::ReadAllBytes($script:ProxyFile)
    Assert-Proxy ($bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) 'Saving dropped the byte order mark (it would make its line a setting)'
    Assert-Proxy ([Array]::IndexOf($bytes, [byte]0xE9) -ge 0) 'Saving changed an ISO 8859-1 character of another line'
    Assert-Proxy (-not (Get-ProxySettings).ContainsKey('proxy')) 'Saving made the byte order mark line a setting'

    # A last line that continues (odd backslashes) must not swallow a new setting.
    [IO.File]::WriteAllLines($script:ProxyFile, [string[]]@('unrelated=a\'), [Text.Encoding]::GetEncoding(28591))
    Set-ProxySetting '' 'direct'
    Assert-Proxy ((Get-ProxySettings)['proxy'] -ceq 'direct') 'A new setting was read as a continuation of the line before it'
    [IO.File]::WriteAllLines($script:ProxyFile, [string[]]@('unrelated=a\\'), [Text.Encoding]::GetEncoding(28591))
    Set-ProxySetting '' 'direct'
    Assert-Proxy (@([IO.File]::ReadAllLines($script:ProxyFile)).Count -eq 2) 'An even number of backslashes does not continue the line'

    # Ordinal comparison, as String.equals: a soft hyphen is not ignored.
    Assert-Proxy ($null -eq (ConvertTo-ProxyValue "dir$([char]0xAD)ect")) 'A soft hyphen in direct was ignored'
    Assert-Proxy ($null -eq (ConvertTo-ProxyValue "windows$([char]0xAD)")) 'A soft hyphen in windows was ignored'
    # A package name must not end in a line break ('$' matches before one).
    $threw = $false
    try { Set-ProxySetting "com.a`n" 'direct' } catch { $threw = $true }
    Assert-Proxy $threw 'A package ending in a line break was accepted'

    # A \u escape without four hex digits: Properties.load throws, so ProxySetting.load has
    # no settings and ProxySetting.save does not replace the file (it would lose the others).
    $malformed = [string[]]@('proxy.com.ok=direct', 'other=\u12', '#comment \u1')
    Write-ProxyLines $malformed
    Assert-Proxy ((Get-ProxySettings).Count -eq 0) 'A file with a malformed \u escape must have no settings, as ProxySetting.load'
    $threw = $false
    try { Set-ProxySetting 'com.ok' $null } catch { $threw = $true }
    Assert-Proxy $threw 'A file with a malformed \u escape must not be replaced'
    Assert-Proxy ((@([IO.File]::ReadAllLines($script:ProxyFile)) -join "`n") -ceq ($malformed -join "`n")) 'A malformed file was changed'
    # An escaped backslash before u, and \u in a comment, are not \u escapes.
    Write-ProxyLines @('proxy.com.ok=direct', 'other=a\\u12', '#comment \u1')
    Assert-Proxy ((Get-ProxySettings)['proxy.com.ok'] -ceq 'direct') 'An escaped backslash before u was read as a \u escape'
    [IO.File]::WriteAllBytes($script:ProxyFile, $savedFile)

    # Live refresh: a CLI change to an app's setting is shown while the panel stays open and
    # visible, with no other file changed: through the production metadata monitor's real
    # file notification, and through the recovery stamp when no notification arrives.
    $root = $scratch
    $icoFile = Join-Path $scratch 'none.ico'
    $script:AppWindowsFile = Join-Path $scratch 'data/system/app-windows.properties'
    $window = @{ IsVisible = $true; WindowState = 'Normal' }
    $script:ViewState = @{ Device = $null; Microg = $null; Store = $null; AppsSignature = $null; AppFiles = @(); Icons = @{} }
    $script:LiveState = @{ Stamp = $null; AppsStamp = $null; PendingSince = $null; PendingFirst = $null; Tick = 0; WasVisible = $true; RecoveryAt = $null }
    $script:LiveRefreshDebounceMs = 400; $script:LiveRefreshMaximumDelayMs = 2000; $script:LiveRefreshRecoveryMs = 5000
    $script:IsBusy = $false
    $appsList = [System.Windows.Controls.ListBox]::new()
    function Update-DeviceView($running) { }
    function Get-DeviceStatus { $false }
    function Get-MicrogStatus { $false }
    function Update-MicrogView($enabled) { }
    function Update-StoreView($installed) { }
    function Get-SeparateWindowApps { ,[System.Collections.Generic.HashSet[string]]::new() }
    function Get-InstalledApps([hashtable] $ReadStamps = $null) {
        foreach ($name in 'shortcut.properties','icon.ico','no-launcher-activity') {
            $null = Get-FileStamp (Join-Path $root "data/data/com.d/gesher-shell/$name") $ReadStamps
        }
        @([pscustomobject]@{ Package = 'com.d'; Label = 'D'; IconPath = $null; Launchable = $true })
    }
    function Shown-Proxy { @($appsList.ItemsSource)[0].ProxyText }
    function Write-ProxyFile([string[]] $lines) {
        [IO.File]::WriteAllLines($script:ProxyFile, $lines, [Text.Encoding]::GetEncoding(28591))
    }
    # Runs the WPF dispatcher (monitor notifications) and the production 500 ms heartbeat tick.
    function Wait-Shown([string] $text, [int] $seconds) {
        $deadline = [DateTime]::UtcNow.AddSeconds($seconds)
        while ((Shown-Proxy) -cne $text -and [DateTime]::UtcNow -lt $deadline) {
            $frame = [System.Windows.Threading.DispatcherFrame]::new()
            $timer = [System.Windows.Threading.DispatcherTimer]::new()
            $timer.Interval = [TimeSpan]::FromMilliseconds(500)
            $timer.Add_Tick({ $frame.Continue = $false })
            $timer.Start(); [System.Windows.Threading.Dispatcher]::PushFrame($frame); $timer.Stop()
            Invoke-LiveRefreshTick
        }
        return (Shown-Proxy) -ceq $text
    }

    . (Join-Path $repo 'tools/package/gesher-panel-native.ps1')
    $context = [System.Windows.Threading.DispatcherSynchronizationContext]::new(
        [System.Windows.Threading.Dispatcher]::CurrentDispatcher, [System.Windows.Threading.DispatcherPriority]::Background)
    $script:MetadataMonitor = [Gesher.PanelChangeMonitor]::new($context)
    try {
        # As Initialize-PanelMetadata wires it.
        $script:MetadataMonitor.add_Changed({ try { Request-PanelMetadataRefresh } catch { } })
        Refresh-Dashboard
        Assert-Proxy ((Shown-Proxy) -ceq (Get-GesherText 'cp_proxy_app_value' @((Get-GesherText 'cp_proxy_mode_direct')))) 'The app''s proxy is not shown'
        $script:LiveState.RecoveryAt = [DateTime]::UtcNow.AddMinutes(10)   # only the notification can refresh
        $generation = $script:MetadataMonitor.Generation
        # As ProxySetting.save (gesher-app proxy com.d 10.9.9.9:3128) writes it.
        Write-ProxyFile @('#Gesher''s proxy', 'proxy.com.d=10.9.9.9\:3128')
        Assert-Proxy (Wait-Shown 'Proxy: 10.9.9.9:3128' 3) 'An external proxy change was not shown from its file notification'
        Assert-Proxy ($script:MetadataMonitor.Generation -gt $generation) 'proxy.properties is not watched by the metadata monitor'
    } finally { $script:MetadataMonitor.Dispose(); $script:MetadataMonitor = $null }

    # No notifications (a missed event): the recovery stamp, within LiveRefreshRecoveryMs.
    $script:LiveState.RecoveryAt = [DateTime]::UtcNow.AddMilliseconds($script:LiveRefreshRecoveryMs)
    Write-ProxyFile @('#Gesher''s proxy')
    Assert-Proxy (Wait-Shown '' 8) 'A missed proxy removal was not recovered while the panel stayed open'
    $before = Get-StateStamp
    Write-ProxyFile @('#Gesher''s proxy', 'proxy.com.d=direct')
    Assert-Proxy ($before -ne (Get-StateStamp)) 'proxy.properties is absent from the recovery stamp'

    # The editor on real WPF controls.
    $combo = [System.Windows.Controls.ComboBox]::new()
    $address = [System.Windows.Controls.TextBox]::new()
    Initialize-ProxyEditor $combo $address $null
    Assert-Proxy ($combo.Items.Count -eq 3 -and $combo.SelectedItem.Tag -eq 'windows' -and -not $address.IsEnabled) 'Every app''s editor must start at Windows'' settings'
    Initialize-ProxyEditor $combo $address '10.1.1.1:80'
    Assert-Proxy ($combo.Items.Count -eq 3 -and $combo.SelectedItem.Tag -eq 'server' -and $address.Text -eq '10.1.1.1:80' -and $address.IsEnabled) 'A saved server is not shown'
    $address.Text = 'http://10.2.2.2:3128/'
    $choice = Read-ProxyEditor $combo $address
    Assert-Proxy ($choice.Valid -and $choice.Value -ceq '10.2.2.2:3128') 'A server address was not read'
    $address.Text = 'direct'
    Assert-Proxy (-not (Read-ProxyEditor $combo $address).Valid) 'A mode name is not a server address'
    $address.Text = 'nope'
    $choice = Read-ProxyEditor $combo $address
    Assert-Proxy (-not $choice.Valid -and $choice.Text -eq 'nope') 'An invalid address was accepted'
    foreach ($item in $combo.Items) { if ($item.Tag -eq 'direct') { $combo.SelectedItem = $item } }
    Assert-Proxy (-not $address.IsEnabled) 'The address stays editable without a server'
    $choice = Read-ProxyEditor $combo $address
    Assert-Proxy ($choice.Valid -and $choice.Value -eq 'direct') 'Direct was not read'

    $appCombo = [System.Windows.Controls.ComboBox]::new()
    $appAddress = [System.Windows.Controls.TextBox]::new()
    Initialize-ProxyEditor $appCombo $appAddress $null -WithInherit
    Assert-Proxy ($appCombo.Items.Count -eq 4 -and $appCombo.SelectedItem.Tag -eq 'inherit') 'An app without its own setting must use every app''s'
    $choice = Read-ProxyEditor $appCombo $appAddress
    Assert-Proxy ($choice.Valid -and $null -eq $choice.Value) 'Inherit must remove the app''s setting'
    Initialize-ProxyEditor $appCombo $appAddress 'windows' -WithInherit
    Assert-Proxy ($appCombo.SelectedItem.Tag -eq 'windows') 'An app''s Windows'' settings are not shown'
    foreach ($item in $appCombo.Items) { if ($item.Tag -eq 'server') { $appCombo.SelectedItem = $item } }
    Assert-Proxy ($appAddress.IsEnabled) 'The address is not editable for a server'

    Assert-Proxy ((Get-ProxyDisplayText 'direct') -eq (Get-GesherText 'cp_proxy_mode_direct')) 'Direct is not shown by name'
    Assert-Proxy ((Get-ProxyDisplayText '1.2.3.4:5') -eq '1.2.3.4:5') 'A server is not shown as it is'
    Assert-Proxy ((Get-GesherText 'cp_proxy_app_value' @('x')) -eq 'Proxy: x') 'The app value text is missing'
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "PASS: control panel proxy settings ($script:Checks checks)"
