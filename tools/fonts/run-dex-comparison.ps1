# Separately labeled CI diagnostic. Production track46 and its image policy remain unchanged.
param([Parameter(Mandatory)][string] $RuntimeDirectory,
      [Parameter(Mandatory)][string] $OldFrameworkDirectory,
      [Parameter(Mandatory)][string] $FixturesDirectory,
      [Parameter(Mandatory)][ValidateSet('current-utf8','old42076')][string] $RuntimeRole)
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'Font DEX execution is CI only' }
function NoDrive([string] $Path) { ($Path -replace '\\','/') -replace '^[A-Za-z]:','' }
$runtime = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
$framework = (Resolve-Path -LiteralPath $OldFrameworkDirectory).Path
$fixtures = (Resolve-Path -LiteralPath $FixturesDirectory).Path
python tools/fonts/assemble-core-runtime.py --verify --destination $runtime --role $RuntimeRole
if ($LASTEXITCODE -ne 0) { throw 'Comparison assembly ownership/source/hash verification failed' }
foreach ($tree in @($runtime,$framework,$fixtures)) {
  $images = @(Get-ChildItem -LiteralPath $tree -Recurse -File | Where-Object Extension -in '.art','.oat','.vdex')
  if ($images.Count) { throw "Image file present in intentional imageless supplier: $($images.FullName)" }
  if (@(Get-ChildItem -LiteralPath $tree -Recurse | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Linked supplier path' }
}
$base = Join-Path $env:RUNNER_TEMP "font-dex-$RuntimeRole-$([guid]::NewGuid())"
$installed = Join-Path $base 'installed'
$root = Join-Path $base 'android'
$work = Join-Path $base 'unrelated-אב space'
$unicodeRoot = Join-Path $base 'font-root-אב space'
foreach ($dir in @($installed,$work,"$root/system/etc","$root/system/fonts","$root/art","$root/data","$root/i18n/etc/icu","$root/tzdata","$unicodeRoot/etc","$unicodeRoot/fonts")) {
  New-Item -ItemType Directory -Path $dir -Force | Out-Null
}
Copy-Item -Path "$runtime/*" -Destination $installed -Recurse
python tools/fonts/assemble-core-runtime.py --verify --destination $installed --role $RuntimeRole
if ($LASTEXITCODE -ne 0) { throw 'Installed copy changed comparison input ownership/hashes' }
Copy-Item -Path "$installed/fonts/*.ttf" -Destination "$root/system/fonts"
Copy-Item -LiteralPath "$installed/fonts/fonts.xml" -Destination "$root/system/etc/fonts.xml"
Copy-Item -Path "$installed/fonts/*.ttf" -Destination "$unicodeRoot/fonts"
Copy-Item -LiteralPath "$installed/fonts/fonts.xml" -Destination "$unicodeRoot/etc/fonts.xml"
Copy-Item -Path "$installed/icudt*l.dat" -Destination "$root/i18n/etc/icu"
if (Test-Path -LiteralPath "$installed/tzdata") { Copy-Item -Path "$installed/tzdata/*" -Destination "$root/tzdata" -Recurse }
Set-Content -LiteralPath "$work/font-unrelated-canary.txt" -Value 'unrelated-canary' -Encoding ascii -NoNewline
foreach ($parent in @($installed,"$root/system",$unicodeRoot)) {
  Set-Content -LiteralPath "$parent/fontscanary.txt" -Value 'font namespace boundary canary' -Encoding utf8
}
$core = @('core-oj','core-libart','core-icu4j') | ForEach-Object { Join-Path $installed "$_.jar" }
$managed = @($core) + @('framework','gesher-app' | ForEach-Object { Join-Path $framework "$_.jar" })
foreach ($file in @($core)+@($managed)+@("$installed/dalvikvm.exe","$fixtures/native2-test.dex")) {
  if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing actual input: $file" }
  Write-Host "FONT_DIAGNOSTIC_INPUT role=$RuntimeRole path=$file sha256=$((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant())"
}
$missingImage = Join-Path $base 'intentionally-absent-image/boot.art'
if (Test-Path -LiteralPath $missingImage) { throw 'Diagnostic image path exists' }
Write-Host "FONT_IMAGE_POLICY role=$RuntimeRole intentional_imageless=true supplied_images=0 nonexistent_image=$missingImage production_policy=UNCHANGED"

# Keep every VM option as an individual value; a concatenation inside a
# comma-expression can otherwise turn the remaining array into one BCP string.
function New-FontChildArguments([string[]] $Bcp, [string[]] $Tail) {
  $values = [Collections.Generic.List[string]]::new()
  $values.Add('-Xbootclasspath:' + (($Bcp | ForEach-Object { NoDrive $_ }) -join ':'))
  $values.Add('-Ximage:' + (NoDrive $missingImage))
  foreach ($option in @('-Xint','-Xusejit:false','-verbose:image','-cp')) { $values.Add($option) }
  $values.Add((NoDrive "$fixtures/native2-test.dex"))
  foreach ($argument in $Tail) { $values.Add($argument) }
  $values.ToArray()
}

function Invoke-FontChild([string] $Role, [string[]] $Bcp, [string[]] $Tail) {
  $psi = [Diagnostics.ProcessStartInfo]::new("$installed/dalvikvm.exe")
  [string[]] $childArguments = @(New-FontChildArguments $Bcp $Tail)
  if ($childArguments.Count -ne 7 + $Tail.Count -or $childArguments[2] -cne '-Xint' -or
      $childArguments[5] -cne '-cp') { throw 'Diagnostic VM argument shape mismatch' }
  foreach ($arg in $childArguments) { $psi.ArgumentList.Add($arg) }
  if ($psi.ArgumentList.Count -ne $childArguments.Count) { throw 'Actual argument count changed' }
  for ($i=0; $i -lt $childArguments.Count; $i++) {
    if ($psi.ArgumentList[$i] -cne $childArguments[$i]) { throw 'Actual ArgumentList value changed' }
  }
  $psi.WorkingDirectory = $work; $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
  # Override only this child's explicitly diagnostic image context.
  $null = $psi.Environment.Remove('GESHER_REQUIRE_BOOT_IMAGE')
  foreach ($entry in @{'ANDROID_ROOT'="$root/system";'ANDROID_ART_ROOT'="$root/art";
      'ANDROID_DATA'="$root/data";'ANDROID_I18N_ROOT'="$root/i18n";'ANDROID_TZDATA_ROOT'="$root/tzdata"}.GetEnumerator()) {
    $psi.Environment[$entry.Key] = NoDrive $entry.Value
  }
  $psi.Environment['GESHER_CI_PROCESS_TRACE'] = '1'
  $receipt = @{runtime_role=$RuntimeRole; child_role=$Role; argv=$childArguments; argv_count=$childArguments.Count; actual_argument_list=@($psi.ArgumentList); cwd=$work;
    android_root=$psi.Environment['ANDROID_ROOT']; require_boot_image=$psi.Environment['GESHER_REQUIRE_BOOT_IMAGE'];
    supplied_images=0; old_framework_source='42076a8f28c0d23eee5b10ee0acf0844be0dc277'}
  Write-Host ('FONT_DIAGNOSTIC_CHILD '+($receipt | ConvertTo-Json -Depth 8 -Compress))
  $process = [Diagnostics.Process]::Start($psi)
  $handle = $process.SafeHandle; $nativeId = $process.Id
  try {
    $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
    $timedOut = -not $process.WaitForExit(120000)
    if ($timedOut) { try { $process.Kill($true) } catch {}; $null=$process.WaitForExit(5000) }
    $pipesDone = $stdout.Wait(10000) -and $stderr.Wait(10000)
    if ($timedOut -or -not $pipesDone) { throw "Bounded font child timeout/incomplete pipes: $Role pid=$nativeId" }
    $text = $stdout.Result + "`n" + $stderr.Result
    Write-Host $text
    if ($text -match 'gesher: boot image loaded:' -or $text -notmatch 'Attempting to fall back to imageless running') { throw "Imageless runtime evidence missing: $Role" }
    [pscustomobject]@{exit=$process.ExitCode; text=$text}
  } finally {
    if (-not $process.HasExited) { try {$process.Kill($true)} catch {}; $null=$process.WaitForExit(5000) }
    $process.Dispose()
    Write-Host "FONT_DIAGNOSTIC_CLEANUP pid=$nativeId handle_closed=$($handle.IsClosed)"
  }
}
$failures = 0
# ASCII numeric witnesses measure Java string contents independently of console rendering.
$echo = Invoke-FontChild 'CLI_UNICODE_ARGUMENTS' $core @('gesher.Native3CliArgumentsTest','אב','word with space','quote"inside','tail\\',([char]::ConvertFromUtf32(0x1f600)),'')
$echoPass = $echo.exit -eq 0 -and $echo.text.Contains('FONT_CLI_UNICODE_PASS hebrew spaces quote backslashes supplementary empty')
$numeric = @([regex]::Matches($echo.text,'(?m)^FONT_CLI_ARG index=([0-9]+) utf16_units=([0-9]+) codepoints=([^ \r\n]*) expected=([^\r\n]*)\r?$'))
$asciiExpected = @{'1'='77,6f,72,64,20,77,69,74,68,20,73,70,61,63,65';'2'='71,75,6f,74,65,22,69,6e,73,69,64,65';'3'='74,61,69,6c,5c,5c';'5'=''}
$validEcho = $echo.text.Contains('FONT_CLI_ARG_COUNT 6') -and $numeric.Count -eq 6
for ($i=0; $i -lt $numeric.Count; $i++) {
  if ($numeric[$i].Groups[1].Value -cne [string]$i) { $validEcho=$false }
  if ($asciiExpected.ContainsKey([string]$i) -and $numeric[$i].Groups[3].Value -cne $asciiExpected[[string]$i]) { $validEcho=$false }
}
if ($RuntimeRole -ceq 'current-utf8') {
  if (-not $validEcho -or -not $echoPass) { $failures++; Write-Host 'FONT_CURRENT_CLI_UNICODE_FAIL' }
  else { Write-Host 'FONT_CURRENT_CLI_UNICODE_PASS native_font_nonbmp_path_qualification=false' }
} else {
  $knownOldGap = $echo.exit -eq 1 -and $echo.text.Contains('java.lang.IllegalStateException: CLI Unicode/quoting arguments changed') -and $echo.text.Contains('gesher.Native3CliArgumentsTest.main') -and $numeric.Count -eq 6 -and $numeric[0].Groups[3].Value -ceq '3f,3f'
  if (-not $validEcho -or (-not $echoPass -and -not $knownOldGap)) { $failures++; Write-Host 'FONT_OLD_CLI_INVALID_OBSERVATION' }
  else { Write-Host "FONT_OLD_CLI_OBSERVATION unicode_pass=$echoPass conformance=false" }
}
foreach ($abi in @('x86_64','arm64-v8a')) {
  foreach ($case in @('unset','set','unicode','wrong')) {
    $mode = if ($case -eq 'unicode') {'set'} else {$case}
    $fontRoot = if ($case -eq 'unicode') {$unicodeRoot} else {"$root/system"}
    $fontDir = if ($case -eq 'unset') {"$installed/fonts"} else {"$fontRoot/fonts"}
    $xml = if ($case -eq 'unset') {"$fontDir/fonts.xml"} else {"$fontRoot/etc/fonts.xml"}
    $result = Invoke-FontChild "CORE_DEX/$abi/$case" $core @('gesher.Native3StandaloneFontTest',
      (NoDrive "$fixtures/$abi/libfonts_standalone.so"),$mode,(NoDrive $fontRoot),(NoDrive $fontDir),(NoDrive $xml))
    $expectedBcp = ($core | ForEach-Object { NoDrive $_ }) -join ':'
    $observedBcp = [regex]::Matches($result.text, '(?m)^FONT_CORE_BCP ([^\r\n]*)\r?$')
    $receivedBcp = if ($observedBcp.Count -eq 1) { $observedBcp[0].Groups[1].Value } else { $null }
    $exactBcp = $observedBcp.Count -eq 1 -and $receivedBcp -ceq $expectedBcp
    Write-Host ('FONT_CORE_BCP_IDENTITY '+(@{runtime_role=$RuntimeRole;abi=$abi;case=$case;expected=$expectedBcp;received=$receivedBcp;observed_count=$observedBcp.Count;exact_match=$exactBcp} | ConvertTo-Json -Compress))
    $prerequisites = $exactBcp -and $result.text.Contains('FONT_CORE_CLASSES_ABSENT Typeface FontFamily SystemFontMap') -and
      $result.text.Contains('FONT_PROVIDER context=dex exports=18 logical_alias=1 asset_provider=1')
    $passes = $result.exit -eq 0 -and $result.text.Contains("FONT_PASS case=$mode ") -and
      $result.text.Contains("FONT_CORE_DEX_PASS case=$mode")
    if ($RuntimeRole -ceq 'current-utf8') {
      if (-not $prerequisites -or -not $passes) { $failures++; Write-Host "FONT_CURRENT_CORE_FAIL abi=$abi case=$case exit=$($result.exit)" }
      else { Write-Host "FONT_CURRENT_CORE_PASS abi=$abi case=$case" }
    } else {
      # An older observation may be a gap only after actual provider/class checks
      # and the complete unchanged contract's PASS/FAIL summary, never a crash.
      $summary = $result.text -match "FONT_(PASS|FAIL) case=$mode "
      if (-not $prerequisites -or -not $summary -or $result.exit -notin @(0,1)) { $failures++; Write-Host "FONT_OLD_CORE_INVALID_OBSERVATION abi=$abi case=$case exit=$($result.exit)" }
      else { Write-Host "FONT_OLD_CORE_OBSERVATION abi=$abi case=$case contract_pass=$passes conformance=false" }
    }
  }
}
# This framework is explicitly OLD42076. It is a managed before-comparison,
# even when paired with the separately declared current native runtime.
$result = Invoke-FontChild 'MANAGED_DEFAULT_WITH_OLD_FRAMEWORK' $managed @('gesher.Native3DefaultFontTest','--expect-uninitialized')
if ($result.exit -ne 0 -or -not $result.text.Contains('FONT_DEX_DEFAULT_BASELINE_UNINITIALIZED The Typeface is not fully initialized.')) {
  $failures++; Write-Host "FONT_MANAGED_BASELINE_FAIL role=$RuntimeRole exit=$($result.exit)"
} else { Write-Host "FONT_MANAGED_BASELINE_GAP_CONFIRMED native_role=$RuntimeRole framework_role=OLD42076 no_manual_font_initialization=true" }
if ($failures) { throw "Font diagnostic contracts failed: $failures" }
Write-Host "FONT_DEX_COMPARISON_COMPLETE native_role=$RuntimeRole full_qualification=false"
