# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Dot-sourced by the packaged entry points. Explicit UTF-8 also works in Windows PowerShell 5.
$uiDir = if ($PSScriptRoot) { $PSScriptRoot } elseif ($MyInvocation.MyCommand.Path) { Split-Path -Parent $MyInvocation.MyCommand.Path } else { '.' }
$script:GesherUiCatalog = Get-Content -LiteralPath (Join-Path $uiDir 'ui-messages.json') -Raw -Encoding UTF8 | ConvertFrom-Json

function Get-GesherUiLanguage([string] $Tags) {
  foreach ($tag in ($Tags -split ',')) {
    $language = ($tag.Trim().ToLowerInvariant() -split '[-_]', 2)[0]
    if ($language -eq 'iw') { $language = 'he' }
    if ($script:GesherUiCatalog.languages.PSObject.Properties.Name -contains $language) { return $language }
  }
  return 'en'
}

# A background panel helper receives an already selected display language.
# This private override does not change GESHER_LOCALES or Android app locales.
$script:GesherPanelUiLanguage = $env:GESHER_PANEL_UI_LANGUAGE
if ($script:GesherUiCatalog.languages.PSObject.Properties.Name -notcontains $script:GesherPanelUiLanguage) {
  $script:GesherPanelUiLanguage = $null
}
$script:GesherNativeUiLanguages = $null
function Get-GesherNativeUiLanguages {
  # Reflection.Emit works in both Windows PowerShell 5.1 and PowerShell 7 without Add-Type.
  if ($null -eq $script:GesherNativeUiLanguages) {
    $existing = 'Gesher.NativeUiLanguages' -as [type]
    if ($null -ne $existing) {
      $script:GesherNativeUiLanguages = $existing
    } else {
      $name = [Reflection.AssemblyName]::new('Gesher.NativeUiLanguages')
      $access = [Reflection.Emit.AssemblyBuilderAccess]::Run
      if ($PSVersionTable.PSEdition -eq 'Core') {
        $assembly = [Reflection.Emit.AssemblyBuilder]::DefineDynamicAssembly($name, $access)
      } else {
        $assembly = [AppDomain]::CurrentDomain.DefineDynamicAssembly($name, $access)
      }
      $module = $assembly.DefineDynamicModule('NativeUiLanguages')
      $type = $module.DefineType('Gesher.NativeUiLanguages', [Reflection.TypeAttributes]::Public)
      $uintRef = [uint32].MakeByRefType()
      $method = $type.DefinePInvokeMethod('GetUserPreferredUILanguages', 'kernel32.dll',
        [Reflection.MethodAttributes]::Public -bor [Reflection.MethodAttributes]::Static,
        [Reflection.CallingConventions]::Standard, [bool],
        [Type[]]@([uint32], $uintRef, [IntPtr], $uintRef),
        [Runtime.InteropServices.CallingConvention]::Winapi,
        [Runtime.InteropServices.CharSet]::Unicode)
      $method.SetImplementationFlags($method.GetMethodImplementationFlags() -bor [Reflection.MethodImplAttributes]::PreserveSig)
      $script:GesherNativeUiLanguages = $type.CreateType()
    }
  }
  return $script:GesherNativeUiLanguages
}

# Pure merge shared by Read-GesherUiLanguages and deterministic tests; no host settings change.
function Merge-GesherUiLanguages {
  param(
    [object] $ProfileLanguages,
    [string] $DisplayLanguages
  )
  $list = [System.Collections.Generic.List[string]]::new()
  $seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
  $trimChars = [char[]]@(' ', "`t", "`r", "`n")
  $addLanguage = {
    param([string] $Name)
    if ($null -eq $Name) { return }
    $trimmed = $Name.Trim($trimChars)
    if ($trimmed -cmatch '\A[A-Za-z][A-Za-z0-9]*(?:[-_][A-Za-z0-9]+)*\z' -and $seen.Add($trimmed)) {
      $list.Add($trimmed)
    }
  }

  if ($ProfileLanguages -is [string[]]) {
    foreach ($name in $ProfileLanguages) {
      if ([string]::IsNullOrEmpty($name)) { break }
      & $addLanguage $name
    }
  } elseif ($ProfileLanguages -is [string]) {
    & $addLanguage $ProfileLanguages.Split([char]0)[0]
  }

  if (-not [string]::IsNullOrEmpty($DisplayLanguages)) {
    foreach ($name in $DisplayLanguages.Split([char]0)) {
      if ($name.Length -eq 0) { break }
      & $addLanguage $name
    }
  }

  if ($list.Count -gt 0) { return ($list -join ',') }
  return 'en'
}

function Read-GesherUiLanguages {
  $profileLanguages = $null
  try {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Control Panel\International\User Profile')
    if ($null -ne $key) {
      try {
        $kind = $key.GetValueKind('Languages')
        if ($kind -eq [Microsoft.Win32.RegistryValueKind]::String -or
            $kind -eq [Microsoft.Win32.RegistryValueKind]::MultiString) {
          $profileLanguages = $key.GetValue('Languages', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        }
      } finally {
        $key.Dispose()
      }
    }
  } catch { }

  $display = $null
  $native = Get-GesherNativeUiLanguages
  [uint32]$count = 0
  [uint32]$size = 0
  $maxChars = ([int]::MaxValue -shr 1) - 2
  if ($native::GetUserPreferredUILanguages(8, [ref]$count, [IntPtr]::Zero, [ref]$size) -and
      $size -gt 0 -and $size -le $maxChars) {
    [uint32]$capacity = $size
    $buffer = [Runtime.InteropServices.Marshal]::AllocHGlobal(([int]$capacity + 2) * 2)
    try {
      if ($native::GetUserPreferredUILanguages(8, [ref]$count, $buffer, [ref]$size) -and $size -le $capacity) {
        $display = [Runtime.InteropServices.Marshal]::PtrToStringUni($buffer, [int]$size)
      }
    } finally {
      [Runtime.InteropServices.Marshal]::FreeHGlobal($buffer)
    }
  }
  return (Merge-GesherUiLanguages -ProfileLanguages $profileLanguages -DisplayLanguages $display)
}

$script:GesherUiTags = if ($script:GesherPanelUiLanguage) { $script:GesherPanelUiLanguage }
  elseif ([string]::IsNullOrWhiteSpace($env:GESHER_LOCALES)) { Read-GesherUiLanguages }
  else { $env:GESHER_LOCALES }
$script:GesherUiLanguage = Get-GesherUiLanguage $script:GesherUiTags

function Get-GesherText([string] $Key, [object[]] $Values = @()) {
  $entry = $script:GesherUiCatalog.messages.PSObject.Properties[$Key]
  if ($null -eq $entry) { throw "Unknown Gesher UI text: $Key" }
  $template = $entry.Value.PSObject.Properties[$script:GesherUiLanguage].Value
  # One pass: a path or app name containing {1} stays literal.
  $result = [regex]::Replace($template, '\{(\d+)\}', [System.Text.RegularExpressions.MatchEvaluator] {
    param($match)
    $index = [int]$match.Groups[1].Value
    if ($index -ge $Values.Count) { throw "Missing Gesher UI argument: $Key" }
    return [string]$Values[$index]
  })
  return $result
}
