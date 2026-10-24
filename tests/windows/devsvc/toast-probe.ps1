# Windows PowerShell 5.1: can an unregistered-in-Start AUMID show a toast and see it in History?
param([string] $AppId = 'Gesher.Probe.devsvc')
$ErrorActionPreference = 'Continue'
"PS $($PSVersionTable.PSVersion) session $([Diagnostics.Process]::GetCurrentProcess().SessionId) user $env:USERNAME"
$key = "HKCU:\Software\Classes\AppUserModelId\$AppId"
New-Item -Force $key | Out-Null
New-ItemProperty -Force $key -Name DisplayName -Value 'Gesher probe' | Out-Null
[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] | Out-Null
[Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom.XmlDocument, ContentType = WindowsRuntime] | Out-Null
$xml = New-Object Windows.Data.Xml.Dom.XmlDocument
$xml.LoadXml('<toast><visual><binding template="ToastGeneric"><text>Gesher probe</text><text>devsvc feasibility</text></binding></visual></toast>')
$toast = New-Object Windows.UI.Notifications.ToastNotification $xml
$toast.Tag = 'probe1'
$toast.Group = 'gesher'
try {
  $notifier = [Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($AppId)
  "setting: $($notifier.Setting)"
  $notifier.Show($toast)
  'shown'
} catch { "show failed: $_" }
Start-Sleep 2
try {
  $h = [Windows.UI.Notifications.ToastNotificationManager]::History.GetHistory($AppId)
  "history count: $($h.Count)"
  foreach ($t in $h) { "  tag=$($t.Tag) group=$($t.Group)" }
  [Windows.UI.Notifications.ToastNotificationManager]::History.Remove('probe1', 'gesher', $AppId)
  $h = [Windows.UI.Notifications.ToastNotificationManager]::History.GetHistory($AppId)
  "history count after remove: $($h.Count)"
} catch { "history failed: $_" }
Remove-Item -Recurse -Force $key
Set-Clipboard -Value 'gesher clip probe'
"clipboard: $(Get-Clipboard)"
Add-Type -AssemblyName System.Windows.Forms
"power: $([System.Windows.Forms.SystemInformation]::PowerStatus.PowerLineStatus) $([System.Windows.Forms.SystemInformation]::PowerStatus.BatteryChargeStatus)"
"wpn services: " + ((Get-Service WpnService, WpnUserService* -ErrorAction SilentlyContinue | ForEach-Object { "$($_.Name)=$($_.Status)" }) -join ', ')
"explorer: " + [bool](Get-Process explorer -ErrorAction SilentlyContinue)
