# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Gesher Control Center (לוח הבקרה של גשר) - GUI Dashboard for apps and Google Play services.

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase, System.Drawing

# Set explicit AppUserModelID so Windows taskbar groups under Gesher instead of PowerShell host
try {
    $code = '[DllImport("shell32.dll", CharSet=CharSet.Unicode)] public static extern int SetCurrentProcessExplicitAppUserModelID(string appID);'
    $type = Add-Type -MemberDefinition $code -Name "Win32ShellGroup" -Namespace "Gesher" -PassThru
    $type::SetCurrentProcessExplicitAppUserModelID("Gesher.ControlPanel")
} catch { }

$here = if ($PSScriptRoot) { $PSScriptRoot } elseif ($MyInvocation.MyCommand.Path) { Split-Path -Parent $MyInvocation.MyCommand.Path } else { (Resolve-Path (Join-Path $PWD 'tools/package')).Path }
. (Join-Path $here 'gesher-ui.ps1')
. (Join-Path $here 'gesher-update.ps1')
$root = Join-Path $env:LOCALAPPDATA 'Gesher'
$script:UpdateDirectory = Join-Path $root 'updates'

# One Control Center. A second start (the Start menu again, or an app's "App info", which
# the system server answers by writing the package here and opening the panel:
# ControlCenter.java) signals the open panel, which comes to the front and takes the
# request on its live-refresh timer, and exits. The mutex is only a name (never owned);
# a panel closes its handles as its window closes, so a start right after that opens a
# new one. "Reopen" after an update starts the new version while the old one is still
# open: GESHER_PANEL_REOPEN lets that start through.
$script:PanelRequestFile = Join-Path $root 'data/system/control-panel-request'
$script:PanelReopen = ($env:GESHER_PANEL_REOPEN -eq '1')
# Apps whose tasks are windows of their own (ADR 0088; AppWindows.java): separate.<package>=true.
$script:AppWindowsFile = Join-Path $root 'data/system/app-windows.properties'
Remove-Item Env:GESHER_PANEL_REOPEN -ErrorAction SilentlyContinue
$script:PanelInstanceCreated = $false
$script:PanelInstance = [System.Threading.Mutex]::new($false, 'Local\Gesher.ControlPanel', [ref] $script:PanelInstanceCreated)
$script:PanelShowEvent = [System.Threading.EventWaitHandle]::new($false, [System.Threading.EventResetMode]::AutoReset, 'Local\Gesher.ControlPanel.Show')
if (-not $script:PanelInstanceCreated -and -not $script:PanelReopen) {
    $null = $script:PanelShowEvent.Set()
    exit 0
}

$isHe = ($script:GesherUiLanguage -eq 'he')
$flowDirection = if ($isHe) { 'RightToLeft' } else { 'LeftToRight' }

# What the dashboard currently shows ($null = never drawn) and the live-refresh bookkeeping.
# Hashtables are mutated in place so event handlers never need to assign script-scope variables.
$script:IsBusy = $false
$script:ActionState = @{ Worker = $null; Completing = $false; Closed = $false }
$script:ViewState = @{ Device = $null; Microg = $null; Store = $null; AppsSignature = $null; AppFiles = @(); Icons = @{} }
$script:LiveState = @{ Stamp = $null; AppsStamp = $null; PendingSince = $null; Tick = 0 }

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
    if ($pkg -notmatch '^[A-Za-z0-9_.]+$') { return }
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
            if ($p) { return $true }
        }
    }
    return $false
}

# Cheap change detection: length and write time of the files the dashboard derives its state from.
# Polled on the UI thread instead of FileSystemWatcher, so no event crosses threads or runspaces and
# no directory handle is held open inside the device's data directory.
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
    ) -join '|')
}

# Shortcut labels and icons are written after packages.list, so they are tracked separately.
function Get-AppFilesStamp {
    return (@($script:ViewState.AppFiles | ForEach-Object { Get-FileStamp $_ }) -join '|')
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
        $propFile = Join-Path $root "data/data/$pkg/gesher-shell/shortcut.properties"
        if (Test-Path -LiteralPath $propFile) {
            foreach ($propLine in Get-Content -LiteralPath $propFile) {
                if ($propLine -match '^\s*label\s*=\s*(.*)$') {
                    $raw = $Matches[1].Trim()
                    $label = [regex]::Replace($raw, '\\u([0-9a-fA-F]{4})', { param($m) [char][int]"0x$($m.Groups[1].Value)" })
                }
                if ($propLine -match '^\s*icon\s*=\s*(.*)$') {
                    $iconCandidate = $Matches[1].Trim() -replace '\\\\', '\' -replace '\\:', ':'
                    if (Test-Path -LiteralPath $iconCandidate) { $iconPath = $iconCandidate }
                }
            }
        }
        if (-not $iconPath) {
            $candidate = Join-Path $root "data/data/$pkg/gesher-shell/icon.ico"
            if (Test-Path -LiteralPath $candidate) { $iconPath = $candidate }
        }

        $apps.Add([PSCustomObject]@{
            Package = $pkg
            Label = $label
            IconPath = $iconPath
        })
    }
    return $apps
}

# Build WPF Window in XAML
[xml]$xaml = @"
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="$(Get-GesherText 'cp_title')"
        Width="920" Height="820" MinWidth="800" MinHeight="600"
        WindowStartupLocation="CenterScreen"
        FlowDirection="$flowDirection"
        FontFamily="Segoe UI, Tahoma, Arial"
        Background="#F5F6F8"
        AllowDrop="True">
    <Window.Resources>
        <Style TargetType="Button">
            <Setter Property="Padding" Value="14,8"/>
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
            <Style.Triggers>
                <Trigger Property="IsMouseOver" Value="True">
                    <Setter Property="Background" Value="#F2F4F7"/>
                </Trigger>
            </Style.Triggers>
        </Style>
        <Style x:Key="PrimaryButton" TargetType="Button" BasedOn="{StaticResource {x:Type Button}}">
            <Setter Property="Background" Value="#1570EF"/>
            <Setter Property="BorderBrush" Value="#1570EF"/>
            <Setter Property="Foreground" Value="#FFFFFF"/>
            <Style.Triggers>
                <Trigger Property="IsMouseOver" Value="True">
                    <Setter Property="Background" Value="#175CD3"/>
                </Trigger>
            </Style.Triggers>
        </Style>
        <Style x:Key="DangerButton" TargetType="Button" BasedOn="{StaticResource {x:Type Button}}">
            <Setter Property="Background" Value="#FFF1F3"/>
            <Setter Property="BorderBrush" Value="#FDA29B"/>
            <Setter Property="Foreground" Value="#B42318"/>
            <Style.Triggers>
                <Trigger Property="IsMouseOver" Value="True">
                    <Setter Property="Background" Value="#FEE4E2"/>
                </Trigger>
            </Style.Triggers>
        </Style>
    </Window.Resources>
    <ScrollViewer Name="PanelScroll" VerticalScrollBarVisibility="Auto" HorizontalScrollBarVisibility="Disabled">
    <!-- Bound the first measure too, before SizeChanged adjusts the viewport. -->
    <Grid Name="PanelLayout" Margin="24" MinHeight="640" Height="640">
        <Grid.RowDefinitions>
            <RowDefinition Height="Auto"/>
            <RowDefinition Height="Auto"/>
            <RowDefinition Height="Auto"/>
            <RowDefinition Height="*"/>
            <RowDefinition Height="Auto"/>
        </Grid.RowDefinitions>

        <!-- Header -->
        <Border Grid.Row="0" Background="#FFFFFF" BorderBrush="#E4E7EC" BorderThickness="1" CornerRadius="10" Padding="20,16" Margin="0,0,0,16">
            <Grid>
                <Grid.ColumnDefinitions>
                    <ColumnDefinition Width="*"/>
                    <ColumnDefinition Width="Auto"/>
                </Grid.ColumnDefinitions>
                <StackPanel Grid.Column="0">
                    <TextBlock Text="$(Get-GesherText 'cp_title')" FontSize="20" FontWeight="Bold" Foreground="#101828"/>
                    <TextBlock Text="$(Get-GesherText 'cp_subtitle')" FontSize="13" Foreground="#667085" Margin="0,4,0,0"/>
                </StackPanel>
                <StackPanel Grid.Column="1" Orientation="Horizontal" VerticalAlignment="Center">
                    <Border Name="DeviceIndicator" Width="12" Height="12" CornerRadius="6" Background="#98A2B3" Margin="0,0,8,0" VerticalAlignment="Center"/>
                    <TextBlock Name="DeviceStatusText" Text="$(Get-GesherText 'cp_device_stopped')" FontSize="13" FontWeight="SemiBold" Foreground="#344054" VerticalAlignment="Center" Margin="0,0,16,0"/>
                    <Button Name="BtnToggleDevice" Content="$(Get-GesherText 'cp_btn_start')"/>
                </StackPanel>
            </Grid>
        </Border>

        <!-- Services Cards (Google Play & Aurora Store) -->
        <Grid Name="ServicesCards" Grid.Row="1" Margin="0,0,0,16">
            <Grid.ColumnDefinitions>
                <ColumnDefinition Width="*"/>
                <ColumnDefinition Width="16"/>
                <ColumnDefinition Width="*"/>
            </Grid.ColumnDefinitions>

            <!-- Card 1: Google Play (microG) -->
            <Border Grid.Column="0" Background="#FFFFFF" BorderBrush="#E4E7EC" BorderThickness="1" CornerRadius="10" Padding="18">
                <StackPanel>
                    <Grid Margin="0,0,0,10">
                        <Grid.ColumnDefinitions>
                            <ColumnDefinition Width="*"/>
                            <ColumnDefinition Width="Auto"/>
                        </Grid.ColumnDefinitions>
                        <TextBlock Grid.Column="0" Text="$(Get-GesherText 'cp_microg_title')" FontSize="15" FontWeight="Bold" Foreground="#101828"/>
                        <Border Grid.Column="1" Name="MicrogBadge" CornerRadius="12" Padding="8,2" Background="#F2F4F7">
                            <TextBlock Name="MicrogBadgeText" Text="$(Get-GesherText 'cp_microg_disabled')" FontSize="11" FontWeight="SemiBold" Foreground="#475467"/>
                        </Border>
                    </Grid>
                    <TextBlock Text="$(Get-GesherText 'cp_microg_desc')" TextWrapping="Wrap" FontSize="12" Foreground="#667085" Margin="0,0,0,16"/>
                    <WrapPanel Orientation="Horizontal">
                        <Button Name="BtnEnableMicrog" Style="{StaticResource PrimaryButton}" Content="$(Get-GesherText 'cp_btn_enable_microg')" Margin="0,0,8,8"/>
                        <Button Name="BtnGoogleLogin" Content="$(Get-GesherText 'cp_btn_google_login')" Margin="0,0,8,8"/>
                        <Button Name="BtnMicrogSettings" Content="$(Get-GesherText 'cp_btn_microg_settings')" Margin="0,0,8,8"/>
                        <Button Name="BtnDisableMicrog" Style="{StaticResource DangerButton}" Content="$(Get-GesherText 'cp_btn_disable_microg')" Margin="0,0,8,8"/>
                    </WrapPanel>
                </StackPanel>
            </Border>

            <!-- Card 2: App Store (Aurora Store) -->
            <Border Grid.Column="2" Background="#FFFFFF" BorderBrush="#E4E7EC" BorderThickness="1" CornerRadius="10" Padding="18">
                <StackPanel>
                    <Grid Margin="0,0,0,10">
                        <Grid.ColumnDefinitions>
                            <ColumnDefinition Width="*"/>
                            <ColumnDefinition Width="Auto"/>
                        </Grid.ColumnDefinitions>
                        <TextBlock Grid.Column="0" Text="$(Get-GesherText 'cp_store_title')" FontSize="15" FontWeight="Bold" Foreground="#101828"/>
                        <Border Grid.Column="1" Name="StoreBadge" CornerRadius="12" Padding="8,2" Background="#F2F4F7">
                            <TextBlock Name="StoreBadgeText" Text="$(Get-GesherText 'cp_store_not_installed')" FontSize="11" FontWeight="SemiBold" Foreground="#475467"/>
                        </Border>
                    </Grid>
                    <TextBlock Text="$(Get-GesherText 'cp_store_desc')" TextWrapping="Wrap" FontSize="12" Foreground="#667085" Margin="0,0,0,16"/>
                    <WrapPanel Orientation="Horizontal">
                        <Button Name="BtnInstallStore" Style="{StaticResource PrimaryButton}" Content="$(Get-GesherText 'cp_btn_install_store')" Margin="0,0,8,8"/>
                        <Button Name="BtnOpenStore" Content="$(Get-GesherText 'cp_btn_open_store')" Margin="0,0,8,8"/>
                    </WrapPanel>
                </StackPanel>
            </Border>
        </Grid>

        <!-- Runtime updates: network work never runs on the WPF dispatcher. -->
        <Border Grid.Row="2" Background="#FFFFFF" BorderBrush="#E4E7EC" BorderThickness="1" CornerRadius="10" Padding="18,14" Margin="0,0,0,16">
            <Grid>
                <Grid.ColumnDefinitions>
                    <ColumnDefinition Width="*"/>
                    <ColumnDefinition Width="Auto"/>
                </Grid.ColumnDefinitions>
                <StackPanel Grid.Column="0" Margin="0,0,14,0">
                    <TextBlock Text="$(Get-GesherText 'cp_update_title')" FontSize="15" FontWeight="Bold" Foreground="#101828"/>
                    <TextBlock Name="UpdateVersionText" FontSize="12" Foreground="#667085" Margin="0,4,0,0"/>
                    <TextBlock Name="UpdateStatusText" TextWrapping="Wrap" FontSize="12" Foreground="#475467" Margin="0,5,0,0"/>
                    <CheckBox Name="AutomaticUpdateChecks" Content="$(Get-GesherText 'cp_update_automatic')" ToolTip="$(Get-GesherText 'cp_update_automatic_hint')" FontSize="12" Foreground="#475467" Margin="0,9,0,0"/>
                </StackPanel>
                <StackPanel Grid.Column="1" VerticalAlignment="Center">
                    <WrapPanel HorizontalAlignment="Right" MaxWidth="330">
                        <Button Name="BtnCheckUpdate" Content="$(Get-GesherText 'cp_update_check')" Margin="0,0,8,6"/>
                        <Button Name="BtnInstallUpdate" Content="$(Get-GesherText 'cp_update_download_install')" Style="{StaticResource PrimaryButton}" Margin="0,0,0,6" Visibility="Collapsed"/>
                        <Button Name="BtnCancelUpdate" Content="$(Get-GesherText 'cp_update_cancel')" Margin="0,0,0,6" Visibility="Collapsed"/>
                        <Button Name="BtnReopenPanel" Content="$(Get-GesherText 'cp_update_reopen')" Style="{StaticResource PrimaryButton}" Margin="0,0,0,6" Visibility="Collapsed"/>
                    </WrapPanel>
                    <Button Name="BtnUpdateNotes" Content="$(Get-GesherText 'cp_update_notes')" HorizontalAlignment="Right" Padding="8,3" FontSize="12" BorderThickness="0" Foreground="#175CD3" Visibility="Collapsed"/>
                    <ProgressBar Name="UpdateProgress" Height="4" IsIndeterminate="True" Margin="0,8,0,0" Visibility="Collapsed"/>
                </StackPanel>
            </Grid>
        </Border>

        <!-- Installed Applications Section -->
        <Border Name="ApplicationsSection" Grid.Row="3" Background="#FFFFFF" BorderBrush="#E4E7EC" BorderThickness="1" CornerRadius="10" Padding="20">
            <Grid>
                <Grid.RowDefinitions>
                    <RowDefinition Height="Auto"/>
                    <RowDefinition Height="*"/>
                </Grid.RowDefinitions>
                <Grid Grid.Row="0" Margin="0,0,0,16">
                    <Grid.ColumnDefinitions>
                        <ColumnDefinition Width="*"/>
                        <ColumnDefinition Width="Auto"/>
                    </Grid.ColumnDefinitions>
                    <StackPanel Grid.Column="0">
                        <TextBlock Text="$(Get-GesherText 'cp_apps_title')" FontSize="16" FontWeight="Bold" Foreground="#101828"/>
                        <TextBlock Text="$(Get-GesherText 'cp_drag_drop_hint')" FontSize="12" Foreground="#667085" Margin="0,2,0,0"/>
                    </StackPanel>
                    <StackPanel Grid.Column="1" Orientation="Horizontal">
                        <Button Name="BtnInstallApk" Style="{StaticResource PrimaryButton}" Content="$(Get-GesherText 'cp_btn_install_apk')"/>
                    </StackPanel>
                </Grid>

                    <ItemsControl Name="AppsList" Grid.Row="1"
                                  ScrollViewer.CanContentScroll="True"
                                  VirtualizingPanel.IsVirtualizing="True"
                                  VirtualizingPanel.VirtualizationMode="Recycling"
                                  VirtualizingPanel.ScrollUnit="Pixel">
                        <ItemsControl.Template>
                            <ControlTemplate TargetType="ItemsControl">
                                <ScrollViewer Name="AppsViewport" CanContentScroll="True"
                                              VerticalScrollBarVisibility="Auto" HorizontalScrollBarVisibility="Disabled">
                                    <ItemsPresenter Name="ItemsPresenter"/>
                                </ScrollViewer>
                            </ControlTemplate>
                        </ItemsControl.Template>
                        <ItemsControl.ItemsPanel>
                            <ItemsPanelTemplate><VirtualizingStackPanel/></ItemsPanelTemplate>
                        </ItemsControl.ItemsPanel>
                        <ItemsControl.ItemTemplate>
                            <DataTemplate>
                                <Border BorderBrush="#F2F4F7" BorderThickness="0,0,0,1" Padding="12,10" Background="Transparent">
                                    <Grid>
                                        <Grid.ColumnDefinitions>
                                            <ColumnDefinition Width="48"/>
                                            <ColumnDefinition Width="*"/>
                                            <ColumnDefinition Width="Auto"/>
                                        </Grid.ColumnDefinitions>
                                        <Image Grid.Column="0" Source="{Binding IconImage}" Width="36" Height="36" HorizontalAlignment="Center" VerticalAlignment="Center"/>
                                        <StackPanel Grid.Column="1" VerticalAlignment="Center" Margin="12,0,12,0">
                                            <TextBlock Text="{Binding Label}" FontSize="14" FontWeight="SemiBold" Foreground="#101828"/>
                                            <TextBlock Text="{Binding Package}" FontSize="12" Foreground="#667085" Margin="0,2,0,0"/>
                                        </StackPanel>
                                        <StackPanel Grid.Column="2" Orientation="Horizontal" VerticalAlignment="Center">
                                            <CheckBox Content="$(Get-GesherText 'cp_separate_windows')" ToolTip="$(Get-GesherText 'cp_separate_windows_tip')" IsChecked="{Binding SeparateWindows, Mode=OneWay}" Tag="{Binding Package}" VerticalAlignment="Center" Margin="0,0,12,0"/>
                                            <Button Content="$(Get-GesherText 'cp_btn_launch')" Tag="{Binding Package}" Margin="0,0,8,0"/>
                                            <Button Content="$(Get-GesherText 'cp_btn_app_info')" Tag="{Binding Package}" Margin="0,0,8,0"/>
                                            <Button Style="{StaticResource DangerButton}" Content="$(Get-GesherText 'cp_btn_uninstall')" Tag="{Binding Package}"/>
                                        </StackPanel>
                                    </Grid>
                                </Border>
                            </DataTemplate>
                        </ItemsControl.ItemTemplate>
                    </ItemsControl>
            </Grid>
        </Border>

        <!-- Status & Progress Footer -->
        <Border Grid.Row="4" Margin="0,12,0,0">
            <Grid>
                <Grid.ColumnDefinitions>
                    <ColumnDefinition Width="*"/>
                    <ColumnDefinition Width="Auto"/>
                </Grid.ColumnDefinitions>
                <TextBlock Name="StatusMessage" Grid.Column="0" Text="$(Get-GesherText 'cp_status_ready')" FontSize="12" Foreground="#475467" VerticalAlignment="Center"/>
                <ProgressBar Name="ActivityBar" Grid.Column="1" Width="140" Height="6" IsIndeterminate="False" Visibility="Hidden" VerticalAlignment="Center"/>
            </Grid>
        </Border>
    </Grid>
    </ScrollViewer>
</Window>
"@

$reader = [System.Xml.XmlNodeReader]::new($xaml)
$window = [System.Windows.Markup.XamlReader]::Load($reader)
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
$updateVersionText = $window.FindName('UpdateVersionText')
$updateStatusText = $window.FindName('UpdateStatusText')
$automaticUpdateChecks = $window.FindName('AutomaticUpdateChecks')
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
    # keeps a useful viewport rather than being squeezed behind the update card.
    $panelLayout.Height = [Math]::Max(640, $panelScroll.ActualHeight - 48)
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
        foreach ($path in @((Join-Path $shell 'shortcut.properties'), (Join-Path $shell 'icon.ico'), $app.IconPath)) {
            if ($path -and $watched.Add([string]$path)) { $appFiles.Add([string]$path) }
        }
        if ($app.IconPath -and -not $iconStamps.ContainsKey($app.IconPath)) {
            $iconStamps[$app.IconPath] = Get-FileStamp $app.IconPath
        }
        "$($app.Package)`t$($app.Label)`t$($app.IconPath)`t$(if ($app.IconPath) { $iconStamps[$app.IconPath] })"
    }) -join "`n"
    $signature += "`n$($iconStamps[$icoFile])"
    $separateApps = Get-SeparateWindowApps
    $signature += "`n" + (@($separateApps | Sort-Object) -join ',')
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
        $items.Add([PSCustomObject]@{
            Package = $app.Package
            Label = $app.Label
            IconImage = $imgSource
            SeparateWindows = $separateApps.Contains([string]$app.Package)
        })
    }
    $appsList.ItemsSource = $items
    # Retain only icons used by the current view; removed apps release their entries.
    $script:ViewState.Icons = $icons
    $script:ViewState.AppsSignature = $signature
}

# Full re-evaluation; used at load, after every panel action and when live refresh sees a change.
function Refresh-Dashboard {
    # Stamps are taken before reading so a change made during the read is still noticed next tick.
    $stateStamp = Get-StateStamp
    $appsStamp = Get-AppFilesStamp
    # A transient read/render failure must leave a retry pending even when the files have
    # stopped changing. Commit the observed stamps only after the whole refresh succeeds.
    if (-not $script:LiveState.PendingSince) { $script:LiveState.PendingSince = [DateTime]::UtcNow }
    Update-DeviceView (Get-DeviceStatus)
    Update-MicrogView (Get-MicrogStatus)
    $installed = Get-InstalledApps
    Update-StoreView $installed
    Update-AppsView $installed
    $script:LiveState.Stamp = $stateStamp
    $script:LiveState.AppsStamp = $appsStamp
    $script:LiveState.PendingSince = $null
}

# Always release the action guard: a transient refresh failure is retried by the timer.
function Complete-DashboardAction {
    try { Refresh-Dashboard } finally { Set-Busy $false }
}

# Live refresh, driven by one DispatcherTimer on the UI thread (500 ms):
# - every tick: compare the state-file stamp; a change is applied once it has been stable for one
#   tick (>= 400 ms debounce), so a burst of writes from install/start/stop causes one refresh;
# - every 4th tick (2 s): re-check the system_server PID (a crash leaves the ready file behind)
#   and the per-app shortcut/icon files, which are written after packages.list;
# - nothing happens while an action is busy; the action's own final refresh covers that period.
$script:LiveRefreshIntervalMs = 500
$script:LiveRefreshDebounceMs = 400
$script:LiveRefreshSlowEvery = 4

function Invoke-LiveRefreshTick {
    $live = $script:LiveState
    $live.Tick++
    if ($script:IsBusy) { return }
    $now = [DateTime]::UtcNow
    $stamp = Get-StateStamp
    if ($stamp -ne $live.Stamp) {
        $live.Stamp = $stamp
        $live.PendingSince = $now
        return
    }
    if (($live.Tick % $script:LiveRefreshSlowEvery) -eq 0) {
        Update-DeviceView (Get-DeviceStatus)
        $appsStamp = Get-AppFilesStamp
        if ($appsStamp -ne $live.AppsStamp) {
            $live.AppsStamp = $appsStamp
            if (-not $live.PendingSince) { $live.PendingSince = $now }
            return
        }
    }
    if ($live.PendingSince -and ($now - $live.PendingSince).TotalMilliseconds -ge $script:LiveRefreshDebounceMs) {
        Refresh-Dashboard
    }
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
    'android:manage_external_storage' = 'cp_access_all_files'
    'android:system_alert_window' = 'cp_access_overlay'
    'android:request_install_packages' = 'cp_access_install'
    'android:write_settings' = 'cp_access_write_settings'
    'android:get_usage_stats' = 'cp_access_usage'
    'android:manage_media' = 'cp_access_media'
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
function Invoke-PanelSpecialAccess([string[]] $arguments) {
    $appScript = Join-Path $here 'gesher-app.ps1'
    $system = [Environment]::GetFolderPath('System')
    $info = [System.Diagnostics.ProcessStartInfo]::new((Join-Path $system 'WindowsPowerShell\v1.0\powershell.exe'))
    # Every argument was checked against a fixed pattern: no quoting is needed.
    $info.Arguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + $appScript + '" special-access ' + ($arguments -join ' ')
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
function Start-PanelSpecialAccess([string[]] $arguments) {
    $powerShell = [PowerShell]::Create()
    try {
        $body = "param(`$here, `$timeout, `$arguments)`n`$script:SpecialAccessTimeoutMs = `$timeout`n" +
            "function Invoke-PanelSpecialAccess {`n$((Get-Command Invoke-PanelSpecialAccess).Definition)`n}`n" +
            "Invoke-PanelSpecialAccess `$arguments"
        $null = $powerShell.AddScript($body).AddArgument($here).AddArgument($script:SpecialAccessTimeoutMs).AddArgument([string[]]$arguments)
        return [PSCustomObject]@{ PowerShell = $powerShell; Handle = $powerShell.BeginInvoke() }
    } catch {
        $powerShell.Dispose()
        throw
    }
}

function Test-PanelSpecialAccessDone($worker) {
    return [bool]$worker.Handle.IsCompleted
}

function Receive-PanelSpecialAccess($worker) {
    try {
        $results = @($worker.PowerShell.EndInvoke($worker.Handle))
        $last = if ($results.Count) { $results[-1] } else { $null }
        if ($last -and $last.PSObject.Properties['ExitCode']) { return $last }
    } catch { }
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

# The app details shown for Settings requests closed: the apps that asked get their results
# (SettingsResults.java). Fire and forget: nothing waits on the UI thread.
function Send-PanelSettingsClosed([string[]] $tokens) {
    $tokens = @($tokens | Where-Object { $_ -cmatch '^[0-9a-f]{32}$' } | Select-Object -Unique)
    if ($tokens.Count -eq 0) { return }
    $info = [System.Diagnostics.ProcessStartInfo]::new(
        (Join-Path ([Environment]::GetFolderPath('System')) 'WindowsPowerShell\v1.0\powershell.exe'))
    $info.Arguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + (Join-Path $here 'gesher-app.ps1') + '" settings-closed ' + ($tokens -join ' ')
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    try { ([System.Diagnostics.Process]::Start($info)).Dispose() } catch { }
}

# A request from the system server: the package, and the result token of an app waiting
# for the screen's result. $null for anything else.
function ConvertFrom-PanelRequest([string] $text) {
    if ($null -eq $text) { return $null }
    if ($text -cmatch '^([A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+)(?:\t([0-9a-f]{32}))?$' -and $Matches[1].Length -le 255) {
        return [PSCustomObject]@{ Package = $Matches[1]; Token = $(if ($Matches[2]) { $Matches[2] } else { $null }) }
    }
    return $null
}

# FromList: the user opened it from the app list, rather than an app from Settings' screens.
function Show-AppInfoDialog([string] $pkg, [switch] $FromList) {
    $item = $appsList.ItemsSource | Where-Object { $_.Package -eq $pkg } | Select-Object -First 1
    if (-not $item) {
        Refresh-Dashboard
        $item = $appsList.ItemsSource | Where-Object { $_.Package -eq $pkg } | Select-Object -First 1
    }
    if (-not $item) { return }
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
    $uninstall = Show-ModernConfirmDialog `
        -Title $label `
        -SubTitle $pkg `
        -Message $message `
        -Detail (Get-GesherText $(if ($FromList) { 'cp_app_info_detail_list' } else { 'cp_app_info_detail' })) `
        -ConfirmText (Get-GesherText 'cp_btn_uninstall') `
        -CancelText (Get-GesherText 'cp_app_info_close') `
        -IconImage $item.IconImage `
        -CancelIsDefault `
        -SpecialAccessPackage $pkg
    if ($uninstall) { & $script:OnAppUninstall $pkg }
}

# Shows a pending request: brings the panel to the front and, for a package, its details.
# The details shown and the result tokens of the requests they answer: a request for the
# app already shown joins it instead of opening the details again after they close.
$script:ShowingAppInfo = $false
$script:ShowingPackage = $null
$script:ShowingTokens = [Collections.Generic.List[string]]::new()
function Invoke-PanelRequest {
    # Another start signalled: come to the front even while busy or showing a dialog.
    $raise = $false
    if ($script:PanelShowEvent) { try { $raise = $script:PanelShowEvent.WaitOne(0) } catch { } }
    # The dialog runs a nested message loop in which this timer keeps ticking; an action
    # in progress keeps the request until it ends.
    $request = $null
    if ($script:ShowingAppInfo -and $script:ShowingPackage -and -not $script:IsBusy) {
        # Only a request for the app shown is taken now; another one waits for the dialog.
        $pending = $null
        try { $pending = ConvertFrom-PanelRequest ([IO.File]::ReadAllText($script:PanelRequestFile).Trim()) } catch { }
        if ($pending -and $pending.Package -ceq $script:ShowingPackage) {
            $taken = ConvertFrom-PanelRequest (Read-PanelRequest)
            if ($taken -and $taken.Package -ceq $script:ShowingPackage) {
                if ($taken.Token) { $script:ShowingTokens.Add($taken.Token) }
                $raise = $true
            } elseif ($taken -and $taken.Token) {
                # Replaced meanwhile by a request for another app: answer it rather than lose it.
                Send-PanelSettingsClosed @($taken.Token)
            }
        }
    } elseif (-not $script:IsBusy -and -not $script:ShowingAppInfo) { $request = Read-PanelRequest }
    if ($null -ne $request) { $raise = $true }
    if (-not $raise) { return }
    if ($window.WindowState -eq [System.Windows.WindowState]::Minimized) {
        $window.WindowState = [System.Windows.WindowState]::Normal
    }
    # Topmost on and off: Windows lets a background process raise its own window that way.
    $window.Topmost = $true
    $window.Topmost = $false
    $null = $window.Activate()
    $parsed = ConvertFrom-PanelRequest $request
    if ($parsed) {
        $script:ShowingAppInfo = $true
        $script:ShowingPackage = $parsed.Package
        $script:ShowingTokens.Clear()
        if ($parsed.Token) { $script:ShowingTokens.Add($parsed.Token) }
        try { Show-AppInfoDialog $parsed.Package }
        finally {
            $tokens = $script:ShowingTokens.ToArray()
            $script:ShowingTokens.Clear()
            $script:ShowingPackage = $null
            $script:ShowingAppInfo = $false
            if ($tokens.Count) { try { Send-PanelSettingsClosed $tokens } catch { } }
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
        [string] $SpecialAccessPackage = $null
    )

    $badgeBg = if ($IsDanger) { '#FEE4E2' } elseif ($IsWarning) { '#FEF0C7' } else { '#F2F4F7' }
    $badgeFg = if ($IsDanger) { '#D92D20' } elseif ($IsWarning) { '#DC6803' } else { '#344054' }
    $btnStyle = if ($IsDanger) { 'DangerActionButton' } else { 'PrimaryButton' }

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
            <Style.Triggers>
                <Trigger Property="IsMouseOver" Value="True">
                    <Setter Property="Background" Value="#F2F4F7"/>
                </Trigger>
            </Style.Triggers>
        </Style>
        <Style x:Key="PrimaryButton" TargetType="Button" BasedOn="{StaticResource {x:Type Button}}">
            <Setter Property="Background" Value="#1570EF"/>
            <Setter Property="BorderBrush" Value="#1570EF"/>
            <Setter Property="Foreground" Value="#FFFFFF"/>
            <Style.Triggers>
                <Trigger Property="IsMouseOver" Value="True">
                    <Setter Property="Background" Value="#175CD3"/>
                    <Setter Property="BorderBrush" Value="#175CD3"/>
                </Trigger>
            </Style.Triggers>
        </Style>
        <Style x:Key="DangerActionButton" TargetType="Button" BasedOn="{StaticResource {x:Type Button}}">
            <Setter Property="Background" Value="#D92D20"/>
            <Setter Property="BorderBrush" Value="#D92D20"/>
            <Setter Property="Foreground" Value="#FFFFFF"/>
            <Style.Triggers>
                <Trigger Property="IsMouseOver" Value="True">
                    <Setter Property="Background" Value="#B42318"/>
                    <Setter Property="BorderBrush" Value="#B42318"/>
                </Trigger>
            </Style.Triggers>
        </Style>
    </Window.Resources>
    <Border Padding="22">
        <Grid>
            <Grid.RowDefinitions>
                <RowDefinition Height="Auto"/>
                <RowDefinition Height="Auto"/>
                <RowDefinition Height="20"/>
                <RowDefinition Height="Auto"/>
            </Grid.RowDefinitions>

            <Grid Grid.Row="0" Margin="0,0,0,14">
                <Grid.ColumnDefinitions>
                    <ColumnDefinition Width="Auto"/>
                    <ColumnDefinition Width="14"/>
                    <ColumnDefinition Width="*"/>
                </Grid.ColumnDefinitions>
                <Border Name="IconContainer" Grid.Column="0" Width="44" Height="44" CornerRadius="10" Background="$badgeBg" VerticalAlignment="Top">
                    <Image Name="AppIconImage" Width="32" Height="32" HorizontalAlignment="Center" VerticalAlignment="Center" Visibility="Collapsed"/>
                </Border>
                <StackPanel Grid.Column="2" VerticalAlignment="Center">
                    <TextBlock Name="DialogTitle" FontSize="16" FontWeight="Bold" Foreground="#101828"/>
                    <TextBlock Name="DialogSubTitle" FontSize="12" Foreground="#667085" Margin="0,2,0,0" Visibility="Collapsed"/>
                </StackPanel>
            </Grid>

            <StackPanel Grid.Row="1">
                <TextBlock Name="DialogMessage" FontSize="13" FontWeight="SemiBold" Foreground="#344054" TextWrapping="Wrap" Margin="0,0,0,6"/>
                <TextBlock Name="DialogDetail" FontSize="12" Foreground="#667085" TextWrapping="Wrap" Visibility="Collapsed"/>
                <StackPanel Name="SwitchPanel" Margin="0,12,0,0" Visibility="Collapsed">
                    <TextBlock Name="SwitchTitle" FontSize="12" FontWeight="SemiBold" Foreground="#344054" TextWrapping="Wrap" Margin="0,0,0,6"/>
                    <TextBlock Name="SwitchStatus" FontSize="12" Foreground="#667085" TextWrapping="Wrap" Visibility="Collapsed"/>
                    <StackPanel Name="SwitchList"/>
                    <TextBlock Name="SwitchError" FontSize="12" Foreground="#D92D20" TextWrapping="Wrap" Margin="0,6,0,0" Visibility="Collapsed"/>
                </StackPanel>
            </StackPanel>

            <StackPanel Grid.Row="3" Orientation="Horizontal" HorizontalAlignment="Right">
                <Button Name="BtnCancel" IsCancel="True" MinWidth="85"/>
                <Border Width="10"/>
                <Button Name="BtnConfirm" Style="{StaticResource $btnStyle}" IsDefault="True" MinWidth="85"/>
            </StackPanel>
        </Grid>
    </Border>
</Window>
"@
    $reader = [System.Xml.XmlNodeReader]::new($dialogXaml)
    $dialog = [System.Windows.Markup.XamlReader]::Load($reader)
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
        try { $accessState.Query = Start-PanelSpecialAccess @($SpecialAccessPackage) }
        catch {
            $switchStatus.Visibility = 'Collapsed'
            $switchError.Text = Get-GesherText 'cp_access_unavailable'
            $switchError.Visibility = 'Visible'
        }
        $accessTimer = [System.Windows.Threading.DispatcherTimer]::new()
        $accessTimer.Interval = [TimeSpan]::FromMilliseconds(100)
        $accessTimer.Add_Tick({
            if ($accessState.Closed) { return }
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
                                    $worker = Start-PanelSpecialAccess @($SpecialAccessPackage, $op, $(if ($wanted) { 'allow' } else { 'deny' }))
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
        })
        $dialog.Add_Closed({
            $accessTimer.Stop()
            $accessState.Closed = $true
            # Answers still running are disposed by the live timer once they finish.
            if ($accessState.Query) { $script:OrphanSpecialAccessWorkers.Add($accessState.Query); $accessState.Query = $null }
            foreach ($change in @($accessState.Pending.Values)) { $script:OrphanSpecialAccessWorkers.Add($change.Worker) }
            $accessState.Pending.Clear()
        })
        $accessTimer.Start()
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

    $btnConfirm.Add_Click({
        $dialog.DialogResult = $true
        $dialog.Close()
    })
    if ($CancelIsDefault) {
        $btnCancel = $dialog.FindName('BtnCancel')
        $btnConfirm.IsDefault = $false
        $btnCancel.IsDefault = $true
        $dialog.Add_ContentRendered({ $null = $btnCancel.Focus() })
    }

    return ($dialog.ShowDialog() -eq $true)
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

    [xml]$dialogXaml = @"
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Width="440" SizeToContent="Height"
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
            <Style.Triggers>
                <Trigger Property="IsMouseOver" Value="True">
                    <Setter Property="Background" Value="#F2F4F7"/>
                </Trigger>
            </Style.Triggers>
        </Style>
    </Window.Resources>
    <Border Padding="22">
        <Grid>
            <Grid.RowDefinitions>
                <RowDefinition Height="Auto"/>
                <RowDefinition Height="20"/>
                <RowDefinition Height="Auto"/>
            </Grid.RowDefinitions>
            <Grid Grid.Row="0">
                <Grid.ColumnDefinitions>
                    <ColumnDefinition Width="Auto"/>
                    <ColumnDefinition Width="14"/>
                    <ColumnDefinition Width="*"/>
                </Grid.ColumnDefinitions>
                <Border Grid.Column="0" Width="40" Height="40" CornerRadius="20" Background="$badgeBg" VerticalAlignment="Top">
                    <TextBlock Text="$(if ($IsError) { '!' } else { 'i' })" FontWeight="Bold" FontSize="18" Foreground="$badgeFg" HorizontalAlignment="Center" VerticalAlignment="Center"/>
                </Border>
                <ScrollViewer Name="DialogMessageScroll" Grid.Column="2" MaxHeight="400" VerticalScrollBarVisibility="Auto">
                    <TextBlock Name="DialogMessage" FontSize="13" Foreground="#344054" TextWrapping="Wrap" VerticalAlignment="Center"/>
                </ScrollViewer>
            </Grid>
            <StackPanel Grid.Row="2" Orientation="Horizontal" HorizontalAlignment="Right">
                <Button Name="BtnOk" IsDefault="True" IsCancel="True" MinWidth="85"/>
            </StackPanel>
        </Grid>
    </Border>
</Window>
"@
    $reader = [System.Xml.XmlNodeReader]::new($dialogXaml)
    $dialog = [System.Windows.Markup.XamlReader]::Load($reader)
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

# Installs an APK through gesher-app.ps1; on failure throws the panel's message with the
# reason com.gesher.app.Launcher wrote to GESHER_LAUNCH_ERROR_FILE (for example a
# different signing key than the installed app's), not only "failed".
function Install-PanelApk([string] $file, [string] $displayName) {
    $errorFile = $null
    try { $errorFile = [System.IO.Path]::GetTempFileName() } catch { }
    $previous = $env:GESHER_LAUNCH_ERROR_FILE
    if ($errorFile) { $env:GESHER_LAUNCH_ERROR_FILE = $errorFile }
    else { Remove-Item Env:GESHER_LAUNCH_ERROR_FILE -ErrorAction SilentlyContinue }
    $reason = ''
    try {
        & (Join-Path $here 'gesher-app.ps1') install "$file"
        $code = $LASTEXITCODE
    } finally {
        if ($null -eq $previous) { Remove-Item Env:GESHER_LAUNCH_ERROR_FILE -ErrorAction SilentlyContinue }
        else { $env:GESHER_LAUNCH_ERROR_FILE = $previous }
        # Also when gesher-app.ps1 throws: the temporary file never stays behind.
        if ($errorFile) {
            try { $reason = [System.IO.File]::ReadAllText($errorFile, [System.Text.Encoding]::UTF8).Trim() } catch { }
            Remove-Item -LiteralPath $errorFile -Force -ErrorAction SilentlyContinue
        }
    }
    if ($code -ne 0) {
        $message = Get-GesherText 'cp_install_failed' @($displayName)
        if ($reason) { $message += "`n`n" + $reason }
        throw $message
    }
}

# One background owner runs profile mutations. Arguments are data and no WPF
# objects enter its runspace. Existing helper scripts retain their own process
# lifetimes; the panel never aborts a possibly committing install on close.
function New-PanelActionWorker([string] $operation, $request) {
    $powerShell = [PowerShell]::Create()
    try {
        $work = {
            param($here, $operation, $request, $installDefinition, $language)
            $ErrorActionPreference = 'Stop'
            $ProgressPreference = 'SilentlyContinue'
            # This process already has the panel's chosen display language.
            # Reuse it without compiling a native language reader, and without
            # changing GESHER_LOCALES (which also configures Android apps).
            $env:GESHER_PANEL_UI_LANGUAGE = $language
            # Includes nested microG staging helpers. Bound transport waits;
            # committing an installation is deliberately never force-stopped.
            $PSDefaultParameterValues = @{ 'Invoke-WebRequest:TimeoutSec' = 120 }
            . (Join-Path $here 'gesher-ui.ps1')
            $script:GesherUiLanguage = $language
            # Trusted source from this panel, shared with the synchronous helper
            # contracts. No path, label or package is interpolated into source.
            . ([scriptblock]::Create("function Install-PanelApk {`n$installDefinition`n}"))
            function Invoke-PanelApp([string[]] $arguments) {
                $global:LASTEXITCODE = 0
                & (Join-Path $here 'gesher-app.ps1') @arguments 6>$null | Out-Null
                if ($LASTEXITCODE -ne 0) { throw (Get-GesherText 'cp_action_failed' @($LASTEXITCODE)) }
            }
            $failures = [Collections.Generic.List[string]]::new()
            try {
                switch ($operation) {
                    'Device' { Invoke-PanelApp @([string]$request.Command) }
                    'Microg' {
                        Invoke-PanelApp @('shutdown')
                        $global:LASTEXITCODE = 0
                        if ($request.Enable) {
                            & (Join-Path $here 'gesher-app-microg.ps1') -AutoDownload 6>$null | Out-Null
                        } else {
                            & (Join-Path $here 'gesher-app-microg.ps1') -Disable 6>$null | Out-Null
                        }
                        if ($LASTEXITCODE -ne 0) { throw (Get-GesherText 'cp_action_failed' @($LASTEXITCODE)) }
                        Invoke-PanelApp @('start')
                    }
                    'Install' {
                        foreach ($file in $request.Files) {
                            try {
                                Install-PanelApk ([string]$file) ([IO.Path]::GetFileName([string]$file)) 6>$null | Out-Null
                            } catch { $failures.Add($_.Exception.Message) }
                        }
                    }
                    'Store' {
                        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
                        $storeApk = Join-Path ([IO.Path]::GetTempPath()) 'AuroraStore.apk'
                        if (-not (Test-Path -LiteralPath $storeApk)) {
                            $temporary = [IO.Path]::GetTempFileName()
                            try {
                                Invoke-WebRequest -Uri 'https://auroraoss.com/downloads/AuroraStore/Release/AuroraStore-4.6.1.apk' -OutFile $temporary -UseBasicParsing -TimeoutSec 120
                                Move-Item -LiteralPath $temporary -Destination $storeApk -Force
                            } finally {
                                if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
                            }
                        }
                        Install-PanelApk $storeApk 'Aurora Store' 6>$null | Out-Null
                    }
                    'Uninstall' {
                        $global:LASTEXITCODE = 0
                        & (Join-Path $here 'gesher-app.ps1') uninstall ([string]$request.Package) 6>$null | Out-Null
                        if ($LASTEXITCODE -ne 0) { throw (Get-GesherText 'cp_uninstall_failed' @([string]$request.Label)) }
                    }
                    default { throw 'Unknown dashboard operation.' }
                }
            } catch { $failures.Add($_.Exception.Message) }
            [PSCustomObject]@{ Errors = $failures.ToArray() }
        }
        $owner = {
            param($body, $here, $operation, $request, $installDefinition, $language)
            $ErrorActionPreference = 'Stop'
            # ConsoleHost preserves the packaged helpers' native stderr semantics.
            # A bare PowerShell API runspace turns even successful ART diagnostics
            # into terminating errors under ErrorActionPreference=Stop.
            # Declare Unicode Win32 entry points in memory. Runtime CodeDOM compilation
            # uses ANSI-dependent native temporary-path code on some .NET Framework builds.
            # Sequential value types keep pointer-size-dependent layout with the CLR marshaler.
            function Initialize-PanelActionNative {
                if ('Gesher.PanelActionNative' -as [type]) { return }
                $name = [Reflection.AssemblyName]::new('Gesher.PanelActionNative')
                if ($PSVersionTable.PSEdition -eq 'Core') {
                    $assembly = [Reflection.Emit.AssemblyBuilder]::DefineDynamicAssembly($name, [Reflection.Emit.AssemblyBuilderAccess]::Run)
                } else {
                    $assembly = [AppDomain]::CurrentDomain.DefineDynamicAssembly($name, [Reflection.Emit.AssemblyBuilderAccess]::Run)
                }
                $module=$assembly.DefineDynamicModule('Native')
                function Define-Struct([string] $name, $fields) {
                    $builder=$module.DefineType($name,[Reflection.TypeAttributes]'Public,Sealed,SequentialLayout',[ValueType])
                    foreach($name in $fields.Keys) { $null=$builder.DefineField($name,$fields[$name],[Reflection.FieldAttributes]::Public) }
                    $builder.CreateType()
                }
                $security=Define-Struct 'Gesher.PanelActionSecurity' ([ordered]@{Length=[int];Descriptor=[IntPtr];Inherit=[int]})
                $startup=Define-Struct 'Gesher.PanelActionStartup' ([ordered]@{
                    Size=[int];Reserved=[IntPtr];Desktop=[IntPtr];Title=[IntPtr]
                    X=[int];Y=[int];Width=[int];Height=[int];CharsX=[int];CharsY=[int];Fill=[int];Flags=[int]
                    Show=[int16];ReservedSize=[int16];ReservedBytes=[IntPtr];Input=[IntPtr];Output=[IntPtr];Error=[IntPtr]
                })
                $extended=Define-Struct 'Gesher.PanelActionExtendedStartup' ([ordered]@{Startup=$startup;Attributes=[IntPtr]})
                $process=Define-Struct 'Gesher.PanelActionProcessInfo' ([ordered]@{Process=[IntPtr];Thread=[IntPtr];ProcessId=[uint32];ThreadId=[uint32]})
                $builder=$module.DefineType('Gesher.PanelActionNative',[Reflection.TypeAttributes]'Public,Abstract,Sealed')
                $lastError = $builder.DefineField('LastError', [int], [Reflection.FieldAttributes]'Public,Static')
                $threadStatic = [Reflection.Emit.CustomAttributeBuilder]::new([ThreadStaticAttribute].GetConstructor(@()), [object[]]@())
                $lastError.SetCustomAttribute($threadStatic)
                $getLastError = [Runtime.InteropServices.Marshal].GetMethod('GetLastWin32Error')
                $constructor=[Runtime.InteropServices.DllImportAttribute].GetConstructor(@([string]))
                $fields=[Reflection.FieldInfo[]]@([Runtime.InteropServices.DllImportAttribute].GetField('CharSet'),[Runtime.InteropServices.DllImportAttribute].GetField('SetLastError'),[Runtime.InteropServices.DllImportAttribute].GetField('EntryPoint'))
                foreach($signature in @(
                    @{Name='CreateFileW';Return=[IntPtr];Parameters=[Type[]]@([string],[uint32],[uint32],$security.MakeByRefType(),[uint32],[uint32],[IntPtr])},
                    @{Name='InitializeProcThreadAttributeList';Return=[bool];Parameters=[Type[]]@([IntPtr],[int],[uint32],[IntPtr].MakeByRefType())},
                    @{Name='UpdateProcThreadAttribute';Return=[bool];Parameters=[Type[]]@([IntPtr],[uint32],[IntPtr],[IntPtr],[IntPtr],[IntPtr],[IntPtr])},
                    @{Name='DeleteProcThreadAttributeList';Return=[void];Parameters=[Type[]]@([IntPtr])},
                    @{Name='CreateProcessW';Return=[bool];Parameters=[Type[]]@([string],[Text.StringBuilder],[IntPtr],[IntPtr],[bool],[uint32],[IntPtr],[string],$extended.MakeByRefType(),$process.MakeByRefType())},
                    @{Name='WaitForSingleObject';Return=[uint32];Parameters=[Type[]]@([IntPtr],[uint32])},
                    @{Name='GetExitCodeProcess';Return=[bool];Parameters=[Type[]]@([IntPtr],[uint32].MakeByRefType())},
                    @{Name='CloseHandle';Return=[bool];Parameters=[Type[]]@([IntPtr])}
                )) {
                    $values=[object[]]@([Runtime.InteropServices.CharSet]::Unicode,$true,$signature.Name)
                    $attribute=[Reflection.Emit.CustomAttributeBuilder]::new($constructor,[object[]]@('kernel32.dll'),$fields,$values)
                    $native=$builder.DefineMethod(('Native'+$signature.Name),[Reflection.MethodAttributes]'Private,Static,PinvokeImpl',$signature.Return,$signature.Parameters)
                    $native.SetCustomAttribute($attribute)
                    $native.SetImplementationFlags([Reflection.MethodImplAttributes]::PreserveSig)
                    # PowerShell's binder can invoke other APIs before a later
                    # GetLastWin32Error call. Preserve this call's error in managed
                    # IL before crossing that boundary, isolated per native thread.
                    $method=$builder.DefineMethod($signature.Name,[Reflection.MethodAttributes]'Public,Static',$signature.Return,$signature.Parameters)
                    $il=$method.GetILGenerator()
                    for($argument=0;$argument -lt $signature.Parameters.Count;$argument++) {
                        $il.Emit([Reflection.Emit.OpCodes]::Ldarg,[int16]$argument)
                    }
                    $il.Emit([Reflection.Emit.OpCodes]::Call,$native)
                    $result=$null
                    if($signature.Return -ne [void]) {
                        $result=$il.DeclareLocal($signature.Return)
                        $il.Emit([Reflection.Emit.OpCodes]::Stloc,$result)
                    }
                    $il.Emit([Reflection.Emit.OpCodes]::Call,$getLastError)
                    $il.Emit([Reflection.Emit.OpCodes]::Stsfld,$lastError)
                    if($result) { $il.Emit([Reflection.Emit.OpCodes]::Ldloc,$result) }
                    $il.Emit([Reflection.Emit.OpCodes]::Ret)
                }
                $null=$builder.CreateType()
            }

            function Invoke-PanelActionNative([string] $executable,[string] $entry,[string] $here) {
                Initialize-PanelActionNative
                $security=[Gesher.PanelActionSecurity]::new()
                $security.Length=[Runtime.InteropServices.Marshal]::SizeOf([type][Gesher.PanelActionSecurity])
                $security.Inherit=1
                $inputHandle=[IntPtr]::Zero; $outputHandle=[IntPtr]::Zero; $attributes=[IntPtr]::Zero; $handles=[IntPtr]::Zero
                $initialized=$false
                $process=[Gesher.PanelActionProcessInfo]::new()
                function Throw-NativeFailure {
                    $errorCode=[Gesher.PanelActionNative]::LastError
                    throw [ComponentModel.Win32Exception]::new($errorCode)
                }
                function Valid([IntPtr] $handle) { $handle -ne [IntPtr]::Zero -and $handle -ne [IntPtr]::new(-1) }
                try {
                    $inputHandle=[Gesher.PanelActionNative]::CreateFileW('NUL',2147483648,3,[ref]$security,3,0,[IntPtr]::Zero)
                    if(-not (Valid $inputHandle)) { Throw-NativeFailure }
                    $outputHandle=[Gesher.PanelActionNative]::CreateFileW('NUL',1073741824,3,[ref]$security,3,0,[IntPtr]::Zero)
                    if(-not (Valid $outputHandle)) { Throw-NativeFailure }
                    $size=[IntPtr]::Zero
                    $null=[Gesher.PanelActionNative]::InitializeProcThreadAttributeList([IntPtr]::Zero,1,0,[ref]$size)
                    if($size -eq [IntPtr]::Zero) { Throw-NativeFailure }
                    $attributes=[Runtime.InteropServices.Marshal]::AllocHGlobal($size)
                    if(-not [Gesher.PanelActionNative]::InitializeProcThreadAttributeList($attributes,1,0,[ref]$size)) { Throw-NativeFailure }
                    $initialized=$true
                    $handles=[Runtime.InteropServices.Marshal]::AllocHGlobal(2*[IntPtr]::Size)
                    [Runtime.InteropServices.Marshal]::WriteIntPtr($handles,0,$inputHandle)
                    [Runtime.InteropServices.Marshal]::WriteIntPtr($handles,[IntPtr]::Size,$outputHandle)
                    if(-not [Gesher.PanelActionNative]::UpdateProcThreadAttribute($attributes,0,[IntPtr]::new(0x20002),$handles,[IntPtr]::new(2*[IntPtr]::Size),[IntPtr]::Zero,[IntPtr]::Zero)) { Throw-NativeFailure }
                    $startup=[Gesher.PanelActionStartup]::new()
                    $startup.Size=[Runtime.InteropServices.Marshal]::SizeOf([type][Gesher.PanelActionExtendedStartup])
                    $startup.Flags=0x100; $startup.Input=$inputHandle; $startup.Output=$outputHandle; $startup.Error=$outputHandle
                    $extended=[Gesher.PanelActionExtendedStartup]::new()
                    $extended.Startup=$startup; $extended.Attributes=$attributes
                    $command=[Text.StringBuilder]::new('"'+$executable+'" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "'+$entry+'"')
                    if(-not [Gesher.PanelActionNative]::CreateProcessW($executable,$command,[IntPtr]::Zero,[IntPtr]::Zero,$true,0x08080000,[IntPtr]::Zero,$here,[ref]$extended,[ref]$process)) { Throw-NativeFailure }
                    $null=[Gesher.PanelActionNative]::CloseHandle($process.Thread); $process.Thread=[IntPtr]::Zero
                    if([Gesher.PanelActionNative]::WaitForSingleObject($process.Process,[uint32]::MaxValue) -ne 0) { Throw-NativeFailure }
                    $code=[uint32]0
                    if(-not [Gesher.PanelActionNative]::GetExitCodeProcess($process.Process,[ref]$code)) { Throw-NativeFailure }
                    return [int][BitConverter]::ToInt32([BitConverter]::GetBytes($code),0)
                } finally {
                    if(Valid $process.Thread) { $null=[Gesher.PanelActionNative]::CloseHandle($process.Thread) }
                    if(Valid $process.Process) { $null=[Gesher.PanelActionNative]::CloseHandle($process.Process) }
                    if($initialized) { [Gesher.PanelActionNative]::DeleteProcThreadAttributeList($attributes) }
                    if($attributes -ne [IntPtr]::Zero) { [Runtime.InteropServices.Marshal]::FreeHGlobal($attributes) }
                    if($handles -ne [IntPtr]::Zero) { [Runtime.InteropServices.Marshal]::FreeHGlobal($handles) }
                    if(Valid $inputHandle) { $null=[Gesher.PanelActionNative]::CloseHandle($inputHandle) }
                    if(Valid $outputHandle) { $null=[Gesher.PanelActionNative]::CloseHandle($outputHandle) }
                }
            }
            $directory = Join-Path ([IO.Path]::GetTempPath()) ('gesher-panel-action-' + [guid]::NewGuid().ToString('N'))
            $null = [IO.Directory]::CreateDirectory($directory)
            $configuration = Join-Path $directory 'request.json'
            $resultPath = Join-Path $directory 'result.json'
            $entry = Join-Path $directory 'action.ps1'
            try {
                $data = @{ Body = $body; Here = $here; Operation = $operation; Request = $request
                    InstallDefinition = $installDefinition; Language = $language }
                [IO.File]::WriteAllText($configuration, ($data | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($true))
                $hostScript = @'
$ErrorActionPreference = 'Stop'
$resultPath = Join-Path $PSScriptRoot 'result.json'
try {
    $config = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'request.json')) | ConvertFrom-Json
    $result = & ([scriptblock]::Create($config.Body)) $config.Here $config.Operation $config.Request $config.InstallDefinition $config.Language
    [IO.File]::WriteAllText($resultPath, ($result | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($true))
} catch {
    [IO.File]::WriteAllText($resultPath, ([PSCustomObject]@{ Errors = @($_.Exception.Message) } | ConvertTo-Json), [Text.UTF8Encoding]::new($true))
    exit 1
}
'@
                [IO.File]::WriteAllText($entry, $hostScript, [Text.UTF8Encoding]::new($true))
                $executable = Join-Path ([Environment]::GetFolderPath('System')) 'WindowsPowerShell/v1.0/powershell.exe'
                $exitCode = (Invoke-PanelActionNative $executable $entry $here)
                if (-not [IO.File]::Exists($resultPath)) { throw "Dashboard operation exited without a result (exit code $exitCode)." }
                [IO.File]::ReadAllText($resultPath) | ConvertFrom-Json
            } finally {
                # Only this owner's three known files and empty directory are
                # removed, after its native process has exited. No recursive delete.
                foreach ($path in $entry, $configuration, $resultPath) { [IO.File]::Delete($path) }
                [IO.Directory]::Delete($directory, $false)
            }
        }
        $null = $powerShell.AddScript($owner.ToString()).AddArgument($work.ToString()).AddArgument($here).AddArgument($operation).
            AddArgument($request).AddArgument((Get-Command Install-PanelApk).Definition).AddArgument($script:GesherUiLanguage)
        $handle = $powerShell.BeginInvoke()
        return [PSCustomObject]@{ PowerShell = $powerShell; Handle = $handle; Disposed = $false }
    } catch {
        $powerShell.Dispose()
        throw
    }
}

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
    $ofd.Filter = "Android Packages (*.apk;*.apks;*.xapk)|*.apk;*.apks;*.xapk|All Files (*.*)|*.*"
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
        $packages = @($files | Where-Object { [IO.Path]::GetExtension($_).ToLowerInvariant() -in '.apk', '.apks', '.xapk' })
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
        }
    }
})

# Live refresh: reflect devices started/stopped, apps installed and microG toggled outside the panel.
# Handler failures (e.g. a file mid-rewrite) are dropped; the next tick or activation retries.
$script:LiveTimer = [System.Windows.Threading.DispatcherTimer]::new()
$script:LiveTimer.Interval = [TimeSpan]::FromMilliseconds($script:LiveRefreshIntervalMs)
$script:LiveTimer.Add_Tick({
    try { Complete-PanelAction } catch { }
    try { Clear-PanelSpecialAccessWorkers } catch { }
    try { Invoke-LiveRefreshTick } catch { }
    try { Invoke-PanelRequest } catch { }
    try { Invoke-PanelUpdateTick } catch {
        Set-PanelUpdateStatus 'cp_update_check_failed' @() $_.Exception.Message
        Update-PanelUpdateView
    }
})
$window.Add_Activated({
    try { if (-not $script:IsBusy) { Refresh-Dashboard } } catch { }
})
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
    Close-PanelUpdates
    # The window is already closed. Drain the cooperatively cancelled, bounded
    # request here so the worker can delete partial files before its runspace ends.
    # EndInvoke is never allowed to wait on the live WPF dispatcher.
    Complete-PanelUpdateWorker -Wait
    if ($script:UpdateState.Installer) { $script:UpdateState.Installer.Dispose() }
}
