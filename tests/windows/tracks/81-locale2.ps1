# ADR0066 first slice: real LocaleManager/LocaleConfig calls and APK resources.
# All ART/APK execution occurs in CI. Pure Java contracts; no per-NDK-ABI claim.
if (-not $dvmWindows -or -not (Test-Path "$Dir/locale2app.apk") -or
    -not (Test-Path "$Dir/locale2other.apk")) {
  Write-Host 'FAIL  locale2: actual APKs/runtime are missing'
  $failed++
  return
}
$locale2Saved = @{}
foreach ($key in 'ANDROID_DATA', 'GESHER_LOGS', 'GESHER_LOCALES', 'GESHER_SHORTCUTS', 'GESHER_APP_WINDOW') {
  $locale2Saved[$key] = [Environment]::GetEnvironmentVariable($key)
}
$locale2Apk = NoDrive "$Dir/locale2app.apk"
$locale2Pkg = 'com.gesher.locale2app'
$locale2Mode = ''

function Locale2Launch([string] $Activity) {
  if ($locale2Mode -eq 'single') {
    $text = LocaleRun @('com.gesher.app.Main', '--activity', $Activity, $locale2Apk) 300
  } else {
    $text = LocaleRun @('com.gesher.app.Launcher', 'launch', $locale2Apk,
      '--activity', $Activity, '--keep-task', '--headless') 400
  }
  $code = $LASTEXITCODE
  Write-Host $text
  if ($code -ne 0 -or $text.Contains('locale2: FAIL') -or
      -not $text.Contains('locale2: PASS ') -or -not $text.Contains('locale2: DONE ')) {
    throw "actual $locale2Mode/$Activity failed (exit $code)"
  }
  return $text
}

function Locale2Assert([bool] $Condition, [string] $Message) {
  if (-not $Condition) { throw $Message }
}

function Locale2State([string] $Activity, [string] $Override, [string] $Selected,
    [string] $Greeting, [bool] $Callback = $false) {
  $text = Locale2Launch $Activity
  $line = @($text -split "`r?`n" | Where-Object { $_.Contains('locale2: STATE ') }) | Select-Object -Last 1
  Locale2Assert ([bool] $line) 'no actual resource/configuration report'
  $phase = if ($locale2Mode -eq 'single') { 'queued' } else { 'settled' }
  Locale2Assert ((LocaleField $line 'phase') -ceq $phase) "wrong observation boundary: $line"
  $effective = if ($Override -eq 'none') { '[en-US,he-IL,fr-FR]' } else { $Override }
  foreach ($entry in @(@('override', $Override), @('selected', "[$Selected]"),
      @('effective', $effective), @('original', '[en-US,he-IL,fr-FR]'),
      @('greeting', $Greeting))) {
    Locale2Assert ((LocaleField $line $entry[0]) -ceq $entry[1]) "wrong $($entry[0]): $line"
  }
  $first = if ($Selected) { ($Selected -split ',')[0] } else { 'en-US' }
  foreach ($field in 'config', 'default') {
    $tags = (LocaleField $line $field).Trim([char[]]'[]')
    Locale2Assert (($tags -split ',')[0] -ceq $first) "wrong $field first locale: $line"
  }
  if ($Callback) { Locale2Assert ($line -match 'callbacks=[1-9][0-9]*') 'no actual configuration callback' }
  if ($locale2Mode -eq 'single') {
    # Require ALL callbacks queued by the real setters before Main's EndLoop.
    # Missing, duplicated, reordered or deferred delivery must still fail.
    $events = @($text -split "`r?`n" | Where-Object { $_.Contains('locale2: CALLBACK ') })
    $want = @(switch ($Activity) {
      '.MainActivity$Seed' {
        'fr-CA,zh-Hant-TW,he-IL,en-GB,zh-Hans-CN,en-US|Bonjour'
        'fr-CA,zh-Hant-TW,en-GB,en-US|Bonjour'
      }
      '.MainActivity$Equal' { 'he-IL,en-US|Shalom' }
      '.MainActivity$Replace' {
        'he-IL,fr-CA,zh-Hant-TW,en-GB,en-US|Shalom'
        'he-IL,fr-CA,en-US|Shalom'
      }
      '.MainActivity$Clear' {
        'fr-CA,en-US|Bonjour'
        'fr-CA,de-DE,en-US|Bonjour' # Framework resource matching selects available French.
        'fr-CA,en-US|Bonjour'
      }
      '.MainActivity$Empty' { 'he-IL,en-US|Shalom'; 'en-US|Hello' }
      '.MainActivity$NullList' { 'he-IL,en-US|Shalom'; 'en-US|Hello' }
      '.MainActivity$FailureSeed' { 'he-IL,en-US|Shalom' }
      '.MainActivity$Failure' { }
      '.MainActivity$Bounds' { }
      '.MainActivity' { }
      default { throw "No complete single-mode callback oracle for $Activity" }
    })
    Locale2Assert ($events.Count -eq $want.Count -and [int](LocaleField $line 'callbacks') -eq $want.Count) "missing/extra single callbacks ($($events.Count)/$($want.Count)): $line"
    for ($event = 0; $event -lt $want.Count; $event++) {
      $fields = $want[$event].Split('|')
      Locale2Assert ($events[$event].Contains("CALLBACK $($event+1) ") -and
        (LocaleField $events[$event] 'locales') -ceq "[$($fields[0])]" -and
        (LocaleField $events[$event] 'greeting') -ceq $fields[1]) "wrong callback order/payload: $($events[$event])"
    }
  }
  return $line
}

function Locale2Group([string] $Name, [scriptblock] $Body) {
  try {
    & $Body
    Write-Host "PASS  locale2 ($locale2Mode): $Name"
  } catch {
    Write-Host "FAIL  locale2 ($locale2Mode): $Name ($_ )"
    $script:failed++
  }
}

try {
  $env:GESHER_LOCALES = 'en-US' # Controlled system locale; no Windows NLS/sign-in claim.
  $env:GESHER_SHORTCUTS = '0'
  $env:GESHER_APP_WINDOW = 'headless'
  foreach ($locale2Mode in 'single', 'device') {
    $locale2Data = Join-Path $root "data-locale2-$locale2Mode"
    $env:ANDROID_DATA = NoDrive $locale2Data
    $env:GESHER_LOGS = NoDrive (Join-Path $root "logs-locale2-$locale2Mode")
    New-Item -ItemType Directory -Force $locale2Data | Out-Null
    try {
      Locale2Group 'persisted override, language/script matching and ordered selection survive an app restart with actual French resources/defaults' {
        $before = Locale2State '.MainActivity$Seed' '[fr-FR,zh-Hant,en-US]' 'fr-CA,zh-Hant-TW,en-GB' 'Bonjour' $true
        $after = Locale2State '.MainActivity' '[fr-FR,zh-Hant,en-US]' 'fr-CA,zh-Hant-TW,en-GB' 'Bonjour'
        Locale2Assert ((LocaleField $before 'pid') -ne (LocaleField $after 'pid')) 'restart reused the same app PID'
      }
      Locale2Group 'same set in a different order preserves stored order and skips reconciliation, as pinned' {
        $null = Locale2State '.MainActivity$Equal' '[fr-FR,zh-Hant,en-US]' 'he-IL' 'Shalom'
        $null = Locale2State '.MainActivity' '[fr-FR,zh-Hant,en-US]' 'he-IL' 'Shalom'
      }
      Locale2Group 'replacement filters selected locales and survives app/device restart with actual Hebrew resources/defaults' {
        $null = Locale2State '.MainActivity$Replace' '[he-IL,fr-FR]' 'he-IL,fr-CA' 'Shalom' $true
        $null = Locale2State '.MainActivity' '[he-IL,fr-FR]' 'he-IL,fr-CA' 'Shalom'
        if ($locale2Mode -eq 'device') {
          $stop = LocaleRun @('com.gesher.app.Launcher', 'shutdown') 120
          Locale2Assert ($LASTEXITCODE -eq 0) "device shutdown failed: $stop"
          $null = Locale2State '.MainActivity' '[he-IL,fr-FR]' 'he-IL,fr-CA' 'Shalom'
        }
      }
      Locale2Group 'null override restores original APK config, reconciles the selection and stays cleared after restart' {
        $null = Locale2State '.MainActivity$Clear' 'none' 'fr-CA' 'Bonjour'
        $null = Locale2State '.MainActivity' 'none' 'fr-CA' 'Bonjour'
      }
      Locale2Group 'explicit empty override persists and clears selected locales rather than restoring the resource config' {
        $null = Locale2State '.MainActivity$Empty' '[]' '' 'Hello'
        $null = Locale2State '.MainActivity' '[]' '' 'Hello'
      }
      Locale2Group 'non-null config with null supported list persists as explicit empty, including across restart' {
        $null = Locale2State '.MainActivity$NullList' '[]' '' 'Hello'
        $null = Locale2State '.MainActivity' '[]' '' 'Hello'
      }
      Locale2Group 'malformed committed override returns null without exposing a parser exception, including after restart' {
        $target = Join-Path $locale2Data "system/locale_configs/$locale2Pkg.properties"
        $old = [IO.File]::ReadAllBytes($target)
        $bad = [Text.Encoding]::ASCII.GetBytes('locales=\uZZZZ')
        try {
          [IO.File]::WriteAllBytes($target, $bad)
          $null = Locale2State '.MainActivity' 'none' '' 'Hello'
          if ($locale2Mode -eq 'device') {
            $stop = LocaleRun @('com.gesher.app.Launcher', 'shutdown') 120
            Locale2Assert ($LASTEXITCODE -eq 0) "device shutdown failed: $stop"
          }
          $null = Locale2State '.MainActivity' 'none' '' 'Hello'
          Locale2Assert ([Convert]::ToBase64String($bad) -ceq [Convert]::ToBase64String([IO.File]::ReadAllBytes($target))) 'GET changed the malformed committed file'
        } finally { [IO.File]::WriteAllBytes($target, $old) }
        $null = Locale2State '.MainActivity' '[]' '' 'Hello'
      }
      Locale2Group 'real staging failure preserves old override bytes, in-memory state and selected locales after restart' {
        $null = Locale2State '.MainActivity$FailureSeed' '[he-IL,fr-FR]' 'he-IL' 'Shalom'
        $target = Join-Path $locale2Data "system/locale_configs/$locale2Pkg.properties"
        $old = [IO.File]::ReadAllBytes($target)
        $pending = "$target.new"
        New-Item -ItemType Directory -Path $pending | Out-Null
        Set-Content -LiteralPath (Join-Path $pending 'block') -Value 'block staging output'
        try {
          $null = Locale2State '.MainActivity$Failure' '[he-IL,fr-FR]' 'he-IL' 'Shalom'
          Locale2Assert ([Convert]::ToBase64String($old) -ceq [Convert]::ToBase64String([IO.File]::ReadAllBytes($target))) 'failed staging changed committed bytes'
        } finally {
          Remove-Item -LiteralPath (Join-Path $pending 'block')
          Remove-Item -LiteralPath $pending
        }
        $null = Locale2State '.MainActivity' '[he-IL,fr-FR]' 'he-IL' 'Shalom'
      }
      Locale2Group 'real locked-source rename failure preserves old override bytes and skips selected-locale reconciliation' {
        $target = Join-Path $locale2Data "system/locale_configs/$locale2Pkg.properties"
        $old = [IO.File]::ReadAllBytes($target)
        $pending = "$target.new"
        $held = [IO.FileStream]::new($pending, [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite,
          [IO.FileShare]::ReadWrite) # Writes allowed; opening this source with DELETE is denied.
        try {
          $null = Locale2State '.MainActivity$Failure' '[he-IL,fr-FR]' 'he-IL' 'Shalom'
          Locale2Assert ($held.Length -gt 0) 'staging did not write bytes before the rename attempt'
          Write-Host "INFO  locale2 ($locale2Mode): locked staging source contains $($held.Length) newly written bytes"
          Locale2Assert ([Convert]::ToBase64String($old) -ceq [Convert]::ToBase64String([IO.File]::ReadAllBytes($target))) 'failed rename changed committed bytes'
        } finally {
          $held.Dispose()
          Remove-Item -LiteralPath $pending -ErrorAction SilentlyContinue
        }
        $null = Locale2State '.MainActivity' '[he-IL,fr-FR]' 'he-IL' 'Shalom'
      }
      Locale2Group 'actual null, unknown-package, user 1/CURRENT/ALL and CURRENT_OR_SELF API boundaries' {
        $null = Locale2State '.MainActivity$Bounds' '[he-IL,fr-FR]' 'he-IL' 'Shalom'
      }
      if ($locale2Mode -eq 'device') {
        Locale2Group 'distinct registered package may read public supported config but cannot read or change private locales, delegate or clear another package' {
          $text = LocaleRun @('com.gesher.app.Launcher', 'launch', (NoDrive "$Dir/locale2other.apk"), '--headless') 400
          Write-Host $text
          Locale2Assert ($LASTEXITCODE -eq 0 -and -not $text.Contains('locale2other: FAIL') -and
            $text.Contains('locale2other: PASS distinct registered package boundaries')) 'distinct caller controls did not pass'
          $null = Locale2State '.MainActivity' '[he-IL,fr-FR]' 'he-IL' 'Shalom'
        }
      }
    } finally {
      if ($locale2Mode -eq 'device') { $null = LocaleRun @('com.gesher.app.Launcher', 'shutdown') 120 }
    }
  }
} finally {
  foreach ($key in $locale2Saved.Keys) { [Environment]::SetEnvironmentVariable($key, $locale2Saved[$key]) }
}
