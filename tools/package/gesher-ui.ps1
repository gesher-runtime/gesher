# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Dot-sourced by the packaged entry points. Explicit UTF-8 also works in Windows PowerShell 5.
$script:GesherUiCatalog = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'ui-messages.json') -Raw -Encoding UTF8 | ConvertFrom-Json

function Get-GesherUiLanguage([string] $Tags) {
  foreach ($tag in ($Tags -split ',')) {
    $language = ($tag.Trim().ToLowerInvariant() -split '[-_]', 2)[0]
    if ($language -eq 'iw') { $language = 'he' }
    if ($script:GesherUiCatalog.languages.PSObject.Properties.Name -contains $language) { return $language }
  }
  return 'en'
}

if (-not ('Gesher.UiLanguages' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace Gesher {
  public static class UiLanguages {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern bool GetUserPreferredUILanguages(uint flags, out uint count, IntPtr buffer, ref uint size);
    public static string Read() {
      uint count, size = 0;
      if (!GetUserPreferredUILanguages(8, out count, IntPtr.Zero, ref size) || size < 2) return "en";
      IntPtr buffer = Marshal.AllocHGlobal(checked((int)size * 2));
      try {
        if (!GetUserPreferredUILanguages(8, out count, buffer, ref size)) return "en";
        return Marshal.PtrToStringUni(buffer, (int)size).TrimEnd('\0').Replace('\0', ',');
      } finally { Marshal.FreeHGlobal(buffer); }
    }
  }
}
'@
}
$script:GesherUiTags = if ([string]::IsNullOrWhiteSpace($env:GESHER_LOCALES)) { [Gesher.UiLanguages]::Read() } else { $env:GESHER_LOCALES }
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
