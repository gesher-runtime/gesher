# Separately labeled CI diagnostic. Production track46 and its image policy remain unchanged.
param([Parameter(Mandatory)][string] $RuntimeDirectory,
      [Parameter(Mandatory)][string] $OldFrameworkDirectory,
      [Parameter(Mandatory)][string] $FixturesDirectory,
      [Parameter(Mandatory)][ValidateSet('current00f','old42076')][string] $RuntimeRole)
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'Font DEX execution is CI only' }
function NoDrive([string] $Path) { ($Path -replace '\\','/') -replace '^[A-Za-z]:','' }
$runtime = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
$framework = (Resolve-Path -LiteralPath $OldFrameworkDirectory).Path
$fixtures = (Resolve-Path -LiteralPath $FixturesDirectory).Path
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

function Invoke-FontChild([string] $Role, [string[]] $Bcp, [string[]] $Tail) {
  $psi = [Diagnostics.ProcessStartInfo]::new("$installed/dalvikvm.exe")
  $args = @('-Xbootclasspath:'+(($Bcp | ForEach-Object { NoDrive $_ }) -join ':'),
    '-Ximage:'+(NoDrive $missingImage),'-Xint','-Xusejit:false','-verbose:image',
    '-cp',(NoDrive "$fixtures/native2-test.dex")) + $Tail
  foreach ($arg in $args) { $psi.ArgumentList.Add($arg) }
  $psi.WorkingDirectory = $work; $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
  # Override only this child's explicitly diagnostic image context.
  $null = $psi.Environment.Remove('GESHER_REQUIRE_BOOT_IMAGE')
  foreach ($entry in @{'ANDROID_ROOT'="$root/system";'ANDROID_ART_ROOT'="$root/art";
      'ANDROID_DATA'="$root/data";'ANDROID_I18N_ROOT'="$root/i18n";'ANDROID_TZDATA_ROOT'="$root/tzdata"}.GetEnumerator()) {
    $psi.Environment[$entry.Key] = NoDrive $entry.Value
  }
  $psi.Environment['GESHER_CI_PROCESS_TRACE'] = '1'
  $receipt = @{runtime_role=$RuntimeRole; child_role=$Role; argv=$args; cwd=$work;
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
foreach ($abi in @('x86_64','arm64-v8a')) {
  foreach ($case in @('unset','set','unicode','wrong')) {
    $mode = if ($case -eq 'unicode') {'set'} else {$case}
    $fontRoot = if ($case -eq 'unicode') {$unicodeRoot} else {"$root/system"}
    $fontDir = if ($case -eq 'unset') {"$installed/fonts"} else {"$fontRoot/fonts"}
    $xml = if ($case -eq 'unset') {"$fontDir/fonts.xml"} else {"$fontRoot/etc/fonts.xml"}
    $result = Invoke-FontChild "CORE_DEX/$abi/$case" $core @('gesher.Native3StandaloneFontTest',
      (NoDrive "$fixtures/$abi/libfonts_standalone.so"),$mode,(NoDrive $fontRoot),(NoDrive $fontDir),(NoDrive $xml))
    $prerequisites = $result.text.Contains('FONT_CORE_CLASSES_ABSENT Typeface FontFamily SystemFontMap') -and
      $result.text.Contains('FONT_PROVIDER context=dex exports=18 logical_alias=1 asset_provider=1')
    $passes = $result.exit -eq 0 -and $result.text.Contains("FONT_PASS case=$mode ") -and
      $result.text.Contains("FONT_CORE_DEX_PASS case=$mode")
    if ($RuntimeRole -ceq 'current00f') {
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
