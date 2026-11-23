# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Exercise the actual acceptance conditions without launching a Windows runtime.
param([string]$Track = "$PSScriptRoot/tracks/52-surfaces.ps1")
$ErrorActionPreference = 'Stop'
$tokens = $null; $errors = $null
[void][Management.Automation.Language.Parser]::ParseFile($Track, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw ($errors | Out-String) }
$source = [IO.File]::ReadAllText($Track)
$start = $source.IndexOf('    $sharedBitmapProblems = @()')
if ($start -lt 0) { throw 'Bitmap acceptance condition boundary missing' }
$end = $source.IndexOf("    SurfaceCheck 'immutable shared Bitmap", $start)
if ($end -le $start) { throw 'Bitmap result check boundary missing' }
$conditions = $source.Substring($start, $end - $start)
$gate = [scriptblock]::Create('param([string]$sharedBitmapOut, [int]$sharedBitmapCode)' + "`n" +
  $conditions + "`n" + '$sharedBitmapProblems')

$lines = @()
foreach ($cycle in 0..1) {
  foreach ($size in @('9x7', '641x359')) {
    $lines += "shared bitmap: $size cycle $cycle exact pixels after both senders recycled; parent pid 123 relay pid 456"
  }
}
$lines += 'shared bitmap: PASS 8 Bitmap hops through separate Binder process; immutable identity, pixels, metadata and independent retirement verified'
$complete = $lines -join "`n"
if (@(& $gate $complete 0).Count -ne 0) { throw 'Complete declared log fixture must pass' }
$controls = 0
for ($omit = 0; $omit -lt $lines.Count; ++$omit) {
  $missing = @(for ($i = 0; $i -lt $lines.Count; ++$i) {
    if ($i -ne $omit) { $lines[$i] }
  }) -join "`n"
  if (@(& $gate $missing 0).Count -eq 0) { throw "Missing required line $omit passed" }
  ++$controls
}
if (@(& $gate ($complete + "`nshared bitmap: FAILED deliberate") 0).Count -eq 0) {
  throw 'Application failure marker passed'
}
++ $controls
if (@(& $gate ($complete + "`nshared bitmap: FAILED log truncated during launch") 0).Count -eq 0) {
  throw 'Log truncation passed'
}
++ $controls
if (@(& $gate $complete 17).Count -eq 0) { throw 'Nonzero exit passed' }
++ $controls
if (@(& $gate '' 0).Count -eq 0) { throw 'Missing entire output passed' }
++ $controls
Write-Output "Actual Bitmap gate accepted complete log fixture and rejected $controls failure controls"
