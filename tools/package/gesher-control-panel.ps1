# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Gesher Control Center (לוח הבקרה של גשר) - GUI Dashboard for apps and Google Play services.

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase, System.Drawing

$here = if ($PSScriptRoot) { $PSScriptRoot } elseif ($MyInvocation.MyCommand.Path) { Split-Path -Parent $MyInvocation.MyCommand.Path } else { (Resolve-Path (Join-Path $PWD 'tools/package')).Path }
. (Join-Path $here 'gesher-ui.ps1')
$root = Join-Path $env:LOCALAPPDATA 'Gesher'

$isHe = ($script:GesherUiLanguage -eq 'he')
$flowDirection = if ($isHe) { 'RightToLeft' } else { 'LeftToRight' }

# Helper to query device running state
function Get-DeviceStatus {
    $ready = Join-Path $root 'data/system/gesher-device.ready'
    if (Test-Path -LiteralPath $ready) {
        $serverId = 0
        if ([int]::TryParse((Get-Content -LiteralPath $ready -Raw).Trim(), [ref] $serverId)) {
            $p = Get-Process -Id $serverId -ErrorAction SilentlyContinue
            if ($p) { return $true }
        }
    }
    return $false
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
        Width="920" Height="700" MinWidth="800" MinHeight="600"
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
    <Grid Margin="24">
        <Grid.RowDefinitions>
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
        <Grid Grid.Row="1" Margin="0,0,0,16">
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

        <!-- Installed Applications Section -->
        <Border Grid.Row="2" Background="#FFFFFF" BorderBrush="#E4E7EC" BorderThickness="1" CornerRadius="10" Padding="20">
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

                <ScrollViewer Grid.Row="1" VerticalScrollBarVisibility="Auto">
                    <ItemsControl Name="AppsList">
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
                                            <Button Content="$(Get-GesherText 'cp_btn_launch')" Tag="{Binding Package}" Margin="0,0,8,0"/>
                                            <Button Style="{StaticResource DangerButton}" Content="$(Get-GesherText 'cp_btn_uninstall')" Tag="{Binding Package}"/>
                                        </StackPanel>
                                    </Grid>
                                </Border>
                            </DataTemplate>
                        </ItemsControl.ItemTemplate>
                    </ItemsControl>
                </ScrollViewer>
            </Grid>
        </Border>

        <!-- Status & Progress Footer -->
        <Border Grid.Row="3" Margin="0,12,0,0">
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
</Window>
"@

$reader = [System.Xml.XmlNodeReader]::new($xaml)
$window = [System.Windows.Markup.XamlReader]::Load($reader)

# Set icon if available
$icoFile = Join-Path $here 'gesher.ico'
if (Test-Path -LiteralPath $icoFile) {
    try {
        $window.Icon = [System.Windows.Media.Imaging.BitmapFrame]::Create([System.Uri]::new($icoFile))
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

function Set-Busy([bool] $busy, [string] $msg = '') {
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

function Refresh-Dashboard {
    # 1. Device status
    $deviceRunning = Get-DeviceStatus
    if ($deviceRunning) {
        $deviceIndicator.Background = [System.Windows.Media.BrushConverter]::new().ConvertFromString('#12B76A')
        $deviceStatusText.Text = Get-GesherText 'cp_device_running'
        $btnToggleDevice.Content = Get-GesherText 'cp_btn_stop'
    } else {
        $deviceIndicator.Background = [System.Windows.Media.BrushConverter]::new().ConvertFromString('#98A2B3')
        $deviceStatusText.Text = Get-GesherText 'cp_device_stopped'
        $btnToggleDevice.Content = Get-GesherText 'cp_btn_start'
    }

    # 2. microG status
    $microgEnabled = Get-MicrogStatus
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

    # 3. Aurora Store status
    $installed = Get-InstalledApps
    $hasStore = ($installed | Where-Object { $_.Package -eq 'com.aurora.store' }) -ne $null
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

    # 4. Applications List
    $items = [System.Collections.Generic.List[PSObject]]::new()
    $defaultIco = if (Test-Path -LiteralPath $icoFile) { $icoFile } else { $null }
    foreach ($app in $installed) {
        $imgSource = $null
        $resolvedIco = if ($app.IconPath -and (Test-Path -LiteralPath $app.IconPath)) { $app.IconPath } else { $defaultIco }
        if ($resolvedIco) {
            try {
                $bmp = [System.Windows.Media.Imaging.BitmapImage]::new()
                $bmp.BeginInit()
                $bmp.UriSource = [System.Uri]::new($resolvedIco)
                $bmp.CacheOption = [System.Windows.Media.Imaging.BitmapCacheOption]::OnLoad
                $bmp.EndInit()
                $bmp.Freeze()
                $imgSource = $bmp
            } catch { }
        }
        $items.Add([PSCustomObject]@{
            Package = $app.Package
            Label = $app.Label
            IconImage = $imgSource
        })
    }
    $appsList.ItemsSource = $items
}

# Toggle device handler
$btnToggleDevice.Add_Click({
    Set-Busy $true
    try {
        if (Get-DeviceStatus) {
            & (Join-Path $here 'gesher-app.ps1') shutdown
        } else {
            & (Join-Path $here 'gesher-app.ps1') start
        }
    } finally {
        Refresh-Dashboard
        Set-Busy $false
    }
})

# microG Handlers
$btnEnableMicrog.Add_Click({
    Set-Busy $true (Get-GesherText 'cp_staging')
    $window.Dispatcher.Invoke([Action]{}, [System.Windows.Threading.DispatcherPriority]::Render)
    try {
        & (Join-Path $here 'gesher-app.ps1') shutdown
        & (Join-Path $here 'gesher-app-microg.ps1') -AutoDownload
        & (Join-Path $here 'gesher-app.ps1') start
    } catch {
        [System.Windows.MessageBox]::Show($_.Exception.Message, "Gesher", [System.Windows.MessageBoxButton]::OK, [System.Windows.MessageBoxImage]::Error)
    } finally {
        Refresh-Dashboard
        Set-Busy $false
    }
})

$btnGoogleLogin.Add_Click({
    & (Join-Path $here 'gesher-launch.exe') launch com.google.android.gms
})

$btnMicrogSettings.Add_Click({
    & (Join-Path $here 'gesher-launch.exe') launch com.google.android.gms
})

$btnDisableMicrog.Add_Click({
    Set-Busy $true
    try {
        & (Join-Path $here 'gesher-app.ps1') shutdown
        & (Join-Path $here 'gesher-app-microg.ps1') -Disable
        & (Join-Path $here 'gesher-app.ps1') start
    } finally {
        Refresh-Dashboard
        Set-Busy $false
    }
})

# Store Handlers
$btnInstallStore.Add_Click({
    Set-Busy $true (Get-GesherText 'cp_downloading' @('Aurora Store'))
    $window.Dispatcher.Invoke([Action]{}, [System.Windows.Threading.DispatcherPriority]::Render)
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $storeApk = Join-Path $env:TEMP 'AuroraStore.apk'
        if (-not (Test-Path $storeApk)) {
            Invoke-WebRequest -Uri 'https://auroraoss.com/downloads/AuroraStore/Release/AuroraStore-4.6.1.apk' -OutFile $storeApk -UseBasicParsing
        }
        & (Join-Path $here 'gesher-app.ps1') install "$storeApk"
    } catch {
        [System.Windows.MessageBox]::Show($_.Exception.Message, "Gesher", [System.Windows.MessageBoxButton]::OK, [System.Windows.MessageBoxImage]::Error)
    } finally {
        Refresh-Dashboard
        Set-Busy $false
    }
})

$btnOpenStore.Add_Click({
    & (Join-Path $here 'gesher-launch.exe') launch com.aurora.store
})

# Install APK Button Handler
$btnInstallApk.Add_Click({
    $ofd = [Microsoft.Win32.OpenFileDialog]::new()
    $ofd.Filter = "Android Packages (*.apk;*.apks;*.xapk)|*.apk;*.apks;*.xapk|All Files (*.*)|*.*"
    if ($ofd.ShowDialog() -eq $true) {
        $file = $ofd.FileName
        Set-Busy $true (Get-GesherText 'cp_installing' @([System.IO.Path]::GetFileName($file)))
        $window.Dispatcher.Invoke([Action]{}, [System.Windows.Threading.DispatcherPriority]::Render)
        try {
            & (Join-Path $here 'gesher-app.ps1') install "$file"
        } catch {
            [System.Windows.MessageBox]::Show($_.Exception.Message, "Gesher", [System.Windows.MessageBoxButton]::OK, [System.Windows.MessageBoxImage]::Error)
        } finally {
            Refresh-Dashboard
            Set-Busy $false
        }
    }
})

# Drag & Drop Handler
$window.Add_Drop({
    param($sender, $e)
    if ($e.Data.GetDataPresent([System.Windows.DataFormats]::FileDrop)) {
        $files = $e.Data.GetData([System.Windows.DataFormats]::FileDrop)
        foreach ($file in $files) {
            $ext = [System.IO.Path]::GetExtension($file).ToLower()
            if ($ext -in '.apk', '.apks', '.xapk') {
                Set-Busy $true (Get-GesherText 'cp_installing' @([System.IO.Path]::GetFileName($file)))
                $window.Dispatcher.Invoke([Action]{}, [System.Windows.Threading.DispatcherPriority]::Render)
                try {
                    & (Join-Path $here 'gesher-app.ps1') install "$file"
                } catch {
                    [System.Windows.MessageBox]::Show($_.Exception.Message, "Gesher", [System.Windows.MessageBoxButton]::OK, [System.Windows.MessageBoxImage]::Error)
                }
            }
        }
        Refresh-Dashboard
        Set-Busy $false
    }
})

# Per-app Launch and Uninstall handlers (invoked via event binding or code behind)
$script:OnAppLaunch = {
    param($pkg)
    & (Join-Path $here 'gesher-launch.exe') launch $pkg
}

$script:OnAppUninstall = {
    param($pkg)
    $confirm = [System.Windows.MessageBox]::Show(
        (Get-GesherText 'cp_uninstall_confirm' @($pkg)),
        (Get-GesherText 'cp_title'),
        [System.Windows.MessageBoxButton]::YesNo,
        [System.Windows.MessageBoxImage]::Question
    )
    if ($confirm -eq [System.Windows.MessageBoxResult]::Yes) {
        Set-Busy $true
        try {
            & (Join-Path $here 'gesher-app.ps1') uninstall $pkg
        } finally {
            Refresh-Dashboard
            Set-Busy $false
        }
    }
}

# Attach click events through visual tree dispatcher
$appsList.AddHandler([System.Windows.Controls.Button]::ClickEvent, [System.Windows.RoutedEventHandler]{
    param($sender, $e)
    $btn = $e.OriginalSource -as [System.Windows.Controls.Button]
    if ($btn -and $btn.Tag) {
        $pkg = [string]$btn.Tag
        if ($btn.Content -eq (Get-GesherText 'cp_btn_launch')) {
            & $script:OnAppLaunch $pkg
        } elseif ($btn.Content -eq (Get-GesherText 'cp_btn_uninstall')) {
            & $script:OnAppUninstall $pkg
        }
    }
})

# Initial load
Refresh-Dashboard
$window.ShowDialog() | Out-Null
