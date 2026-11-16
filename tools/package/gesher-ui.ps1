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
if (-not $script:GesherPanelUiLanguage -and -not ('Gesher.UiLanguages' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Microsoft.Win32;
namespace Gesher {
  public static class UiLanguages {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern bool GetUserPreferredUILanguages(uint flags, out uint count, IntPtr buffer, ref uint size);
    private static void AddLanguage(List<string> list, HashSet<string> seen, string name) {
      if (name == null) return;
      name = name.Trim(' ', '\t', '\r', '\n');
      if (name.Length == 0 || !((name[0] >= 'A' && name[0] <= 'Z') ||
                                (name[0] >= 'a' && name[0] <= 'z'))) return;
      bool separator = false;
      foreach (char c in name) {
        if (c == '-' || c == '_') {
          if (separator) return;
          separator = true;
        } else {
          if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9'))) return;
          separator = false;
        }
      }
      if (!separator && seen.Add(name)) list.Add(name);
    }

    // Pure merge shared by Read and deterministic tests; no host settings change.
    public static string Merge(object profileLanguages, string displayLanguages) {
      var list = new List<string>();
      var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
      string[] multiple = profileLanguages as string[];
      string single = profileLanguages as string;
      if (multiple != null) {
        foreach (string name in multiple) {
          if (string.IsNullOrEmpty(name)) break;
          AddLanguage(list, seen, name);
        }
      } else if (single != null) {
        AddLanguage(list, seen, single.Split('\0')[0]);
      }
      if (displayLanguages != null) {
        foreach (string name in displayLanguages.Split('\0')) {
          if (name.Length == 0) break;
          AddLanguage(list, seen, name);
        }
      }
      return list.Count > 0 ? string.Join(",", list) : "en";
    }

    public static string Read() {
      object profile = null;
      try {
        using (var key = Registry.CurrentUser.OpenSubKey(@"Control Panel\International\User Profile")) {
          if (key != null) {
            RegistryValueKind kind = key.GetValueKind("Languages");
            if (kind == RegistryValueKind.String || kind == RegistryValueKind.MultiString) {
              profile = key.GetValue("Languages", null, RegistryValueOptions.DoNotExpandEnvironmentNames);
            }
          }
        }
      } catch {}
      string display = null;
      uint count, size = 0;
      if (GetUserPreferredUILanguages(8, out count, IntPtr.Zero, ref size) &&
          size > 0 && size <= (int.MaxValue / 2) - 2) {
        uint capacity = size;
        IntPtr buffer = Marshal.AllocHGlobal(checked(((int)capacity + 2) * 2));
        try {
          if (GetUserPreferredUILanguages(8, out count, buffer, ref size) && size <= capacity) {
            display = Marshal.PtrToStringUni(buffer, (int)size);
          }
        } finally { Marshal.FreeHGlobal(buffer); }
      }
      return Merge(profile, display);
    }

  }
}
'@
}
$script:GesherUiTags = if ($script:GesherPanelUiLanguage) { $script:GesherPanelUiLanguage }
  elseif ([string]::IsNullOrWhiteSpace($env:GESHER_LOCALES)) { [Gesher.UiLanguages]::Read() }
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

function Save-GesherCameraBmp([string] $SourcePath, [string] $DestPath) {
  Add-Type -AssemblyName PresentationCore, WindowsBase
  $uri = [Uri]::new((Resolve-Path -LiteralPath $SourcePath).Path)
  $decoder = [System.Windows.Media.Imaging.BitmapDecoder]::Create(
    $uri,
    [System.Windows.Media.Imaging.BitmapCreateOptions]::None,
    [System.Windows.Media.Imaging.BitmapCacheOption]::OnLoad)
  if (-not $decoder.Frames -or $decoder.Frames.Count -lt 1) {
    throw "Unsupported or empty image: $SourcePath"
  }
  $frame = $decoder.Frames[0]
  [System.Windows.Media.Imaging.BitmapSource] $src = $frame
  $orient = 1
  try {
    $meta = $frame.Metadata -as [System.Windows.Media.Imaging.BitmapMetadata]
    if ($meta -ne $null) {
      if ($meta.ContainsQuery('/app1/ifd/{ushort=274}')) {
        $orient = [int]$meta.GetQuery('/app1/ifd/{ushort=274}')
      } elseif ($meta.ContainsQuery('System.Photo.Orientation')) {
        $orient = [int]$meta.GetQuery('System.Photo.Orientation')
      }
    }
  } catch { }
  $angle = switch ($orient) { 3 { 180 } 6 { 90 } 8 { 270 } default { 0 } }
  $maxSide = 2400.0
  $scale = [Math]::Min(1.0, $maxSide / [Math]::Max($src.PixelWidth, $src.PixelHeight))
  if ($angle -ne 0 -or $scale -lt 0.999) {
    $tg = [System.Windows.Media.TransformGroup]::new()
    if ($scale -lt 0.999) { $tg.Children.Add([System.Windows.Media.ScaleTransform]::new($scale, $scale)) }
    if ($angle -ne 0) { $tg.Children.Add([System.Windows.Media.RotateTransform]::new($angle)) }
    $src = [System.Windows.Media.Imaging.TransformedBitmap]::new($src, $tg)
  }
  $bgr24 = [System.Windows.Media.Imaging.FormatConvertedBitmap]::new($src, [System.Windows.Media.PixelFormats]::Bgr24, $null, 0)
  $encoder = [System.Windows.Media.Imaging.BmpBitmapEncoder]::new()
  $encoder.Frames.Add([System.Windows.Media.Imaging.BitmapFrame]::Create($bgr24))
  $tmp = "$DestPath.tmp"
  $fs = [System.IO.File]::Create($tmp)
  try { $encoder.Save($fs) } finally { $fs.Dispose() }
  Move-Item -LiteralPath $tmp -Destination $DestPath -Force
}
