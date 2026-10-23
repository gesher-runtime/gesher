# The shell track (docs/installer.md): an installed app's label and launcher
# icon, as its Start menu shortcut gets them (com.gesher.app.shell.AppShell
# icon APK OUT.ico), checked as Windows reads the .ico. Dot-sourced by
# run-tests.ps1, whose NoDrive, $Dir, $root and $failed these use. The
# shortcuts themselves are checked against the installed Gesher, in the
# package job (.github/workflows/ci.yml).
. (Join-Path $PSScriptRoot '..\shell\ShellChecks.ps1')

$shellOut = Join-Path $root 'shell-icons'
New-Item -ItemType Directory -Force $shellOut | Out-Null
$bcpShell = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'

# lifecycle.apk: a legacy PNG mipmap, red only at xxxhdpi. components.apk: an
# adaptive icon, a WebP foreground (white only at xxxhdpi) over a blue color,
# in the icon mask. peer.apk has no icon: the framework's default.
$shellApps = @(
  @{ Apk = 'lifecycle'; Package = 'com.gesher.lifecycle'; Label = 'Gesher Lifecycle';
     Source = '^res/mipmap-xxxhdpi[-v0-9]*/ic_launcher\.png$';
     Pixels = @{ '2,2' = 'transparent'; '128,128' = @(0xD3, 0x2F, 0x2F); '128,40' = @(0xD3, 0x2F, 0x2F) } },
  @{ Apk = 'components'; Package = 'com.gesher.components'; Label = 'Gesher Components';
     Source = '^res/mipmap-anydpi[-v0-9]*/ic_launcher\.xml \(adaptive icon: background #ff1e63d6, foreground res/mipmap-xxxhdpi[-v0-9]*/ic_launcher_foreground\.webp \(\w+\)\)';
     Pixels = @{ '2,2' = 'transparent'; '128,128' = @(0xFF, 0xFF, 0xFF); '128,30' = @(0x1E, 0x63, 0xD6) } },
  @{ Apk = 'peer'; Package = 'com.gesher.peer'; Label = $null; Source = '^the framework''s default icon \(no icon\): '; Pixels = @{} }
)
foreach ($app in $shellApps) {
  $ico = Join-Path $shellOut "$($app.Apk).ico"
  $name = "shell: $($app.Apk).apk's label and launcher icon as a multi-size .ico"
  $out = & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpShell" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' com.gesher.app.shell.AppShell icon (NoDrive "$Dir/$($app.Apk).apk") (NoDrive $ico) 2>&1 | Out-String
  $code = $LASTEXITCODE
  $problems = @()
  if ($code -ne 0) { $problems += "exit $code" }
  $iconLine = ($out -split "`n" | Where-Object { $_ -match '^gesher-app: icon of ' } | Select-Object -First 1)
  $iconSource = if ($iconLine) { ($iconLine.Trim() -split ': ', 3)[2] } else { '' }
  if ($iconSource -notmatch $app.Source) { $problems += "the icon's source does not match $($app.Source)" }
  if ($app.Label -and -not $out.Contains("gesher-app: label of $($app.Package): $($app.Label)")) { $problems += "the label is not $($app.Label)" }
  if (-not $out.Contains("gesher-app: AppUserModelID of $($app.Package): Gesher.App.$($app.Package)")) { $problems += 'no AppUserModelID' }
  if (-not $problems) { $problems += @(Test-AppIco $ico $app.Pixels) }
  if (-not $problems) {
    Write-Host "PASS  $name ($iconSource)"
  } else {
    Write-Host "FAIL  $name ($($problems -join '; '))"
    Write-Host $out
    $failed++
  }
}
