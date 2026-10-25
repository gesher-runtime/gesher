# The Microsoft Edge WebView2 Runtime (Evergreen) on this machine
# (docs/webview.md): prints its version from EdgeUpdate's registration and,
# when it is missing (Windows Server images may lack it), installs it with
# Microsoft's bootstrapper (needs administrator rights and the network).
# Dot-source it or run it; $WebView2Version holds the version after.
function Get-WebView2Version {
  $id = '{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}'
  foreach ($key in "HKLM:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\$id",
                   "HKLM:\SOFTWARE\Microsoft\EdgeUpdate\Clients\$id",
                   "HKCU:\Software\Microsoft\EdgeUpdate\Clients\$id") {
    $pv = (Get-ItemProperty -Path $key -Name pv -ErrorAction SilentlyContinue).pv
    if ($pv -and $pv -ne '0.0.0.0') { return $pv }
  }
  return $null
}

$WebView2Version = Get-WebView2Version
if ($WebView2Version) {
  Write-Host "INFO  WebView2 Runtime $WebView2Version is installed"
} else {
  Write-Host 'INFO  no WebView2 Runtime: installing it (Evergreen bootstrapper)'
  $setup = Join-Path ([IO.Path]::GetTempPath()) 'MicrosoftEdgeWebview2Setup.exe'
  try {
    Invoke-WebRequest -Uri 'https://go.microsoft.com/fwlink/p/?LinkId=2124703' -OutFile $setup
    $p = Start-Process $setup -ArgumentList '/silent', '/install' -PassThru -Wait
    Write-Host "INFO  the bootstrapper exited with $($p.ExitCode)"
  } catch {
    Write-Host "INFO  installing the WebView2 Runtime failed: $_"
  }
  $WebView2Version = Get-WebView2Version
  if ($WebView2Version) {
    Write-Host "INFO  WebView2 Runtime $WebView2Version is installed now"
  } else {
    Write-Host 'INFO  the WebView2 Runtime is still missing'
  }
}
