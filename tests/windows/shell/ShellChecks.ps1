# Checks of the shell track's outputs (docs/installer.md): the .ico files
# com.gesher.app.shell.AppIcon writes and the Start menu shortcuts AppShell
# makes. Dot-sourced by tests/windows/tracks/55-shell.ps1 and by the package
# job in .github/workflows/ci.yml.

# The images of an .ico: Width, Height, Png (else a DIB) and Data.
function Read-Ico([string] $Path) {
  $b = [IO.File]::ReadAllBytes($Path)
  if ($b.Length -lt 6 -or [BitConverter]::ToUInt16($b, 0) -ne 0 -or [BitConverter]::ToUInt16($b, 2) -ne 1) {
    throw "$Path is not an .ico (no ICONDIR)"
  }
  $n = [BitConverter]::ToUInt16($b, 4)
  $entries = @()
  for ($i = 0; $i -lt $n; $i++) {
    $o = 6 + 16 * $i
    $w = if ($b[$o] -eq 0) { 256 } else { [int]$b[$o] }
    $h = if ($b[$o + 1] -eq 0) { 256 } else { [int]$b[$o + 1] }
    $size = [BitConverter]::ToUInt32($b, $o + 8)
    $offset = [BitConverter]::ToUInt32($b, $o + 12)
    if ($offset + $size -gt $b.Length) { throw "image $i of $Path lies outside the file" }
    $data = [byte[]]::new($size)
    [Array]::Copy($b, $offset, $data, 0, $size)
    $png = $size -ge 8 -and $data[0] -eq 0x89 -and $data[1] -eq 0x50 -and $data[2] -eq 0x4E -and $data[3] -eq 0x47
    $entries += [pscustomobject]@{ Width = $w; Height = $h; Png = $png; Data = $data }
  }
  return $entries
}

# A pixel of an .ico image: @(A, R, G, B).
function Get-IcoPixel($Entry, [int] $X, [int] $Y) {
  if ($Entry.Png) {
    Add-Type -AssemblyName System.Drawing
    $ms = [IO.MemoryStream]::new($Entry.Data)
    $bmp = [Drawing.Bitmap]::new($ms)
    try {
      if ($bmp.Width -ne $Entry.Width -or $bmp.Height -ne $Entry.Height) {
        throw "a $($bmp.Width)x$($bmp.Height) PNG in a $($Entry.Width)x$($Entry.Height) entry"
      }
      $c = $bmp.GetPixel($X, $Y)
      return @([int]$c.A, [int]$c.R, [int]$c.G, [int]$c.B)
    } finally {
      $bmp.Dispose()
      $ms.Dispose()
    }
  }
  $d = $Entry.Data
  $header = [BitConverter]::ToUInt32($d, 0)
  $w = [BitConverter]::ToInt32($d, 4)
  $h2 = [BitConverter]::ToInt32($d, 8)
  $bpp = [BitConverter]::ToUInt16($d, 14)
  $mask = [int][Math]::Floor(($w + 31) / 32) * 4
  if ($header -ne 40 -or $w -ne $Entry.Width -or $h2 -ne 2 * $Entry.Height -or $bpp -ne 32 -or
      $d.Length -lt 40 + $w * $Entry.Height * 4 + $mask * $Entry.Height) {
    throw "a bad DIB in the $($Entry.Width)x$($Entry.Height) entry (header $header, ${w}x$h2, $bpp bpp, $($d.Length) bytes)"
  }
  $o = 40 + (($Entry.Height - 1 - $Y) * $w + $X) * 4
  return @([int]$d[$o + 3], [int]$d[$o + 2], [int]$d[$o + 1], [int]$d[$o])
}

# What is wrong with an app's .ico (nothing: an empty list): every size AppIcon
# writes, each image readable, System.Drawing.Icon (Windows' icon loader)
# takes its small sizes, and $Pixels (@{ 'x,y' = 'transparent' or @(R, G, B) } at 256x256)
# match; the 16x16 image's center is the 256's.
function Test-AppIco([string] $Path, [hashtable] $Pixels) {
  $problems = @()
  try { $entries = @(Read-Ico $Path) } catch { return @("$_") }
  $sizes = @($entries | ForEach-Object Width)
  foreach ($s in 16, 20, 24, 32, 40, 48, 64, 96, 128, 256) {
    if ($sizes -notcontains $s) { $problems += "no ${s}x$s image" }
  }
  foreach ($e in $entries) {
    try { $null = Get-IcoPixel $e 0 0 } catch { $problems += "the $($e.Width)x$($e.Height) image: $_" }
  }
  Add-Type -AssemblyName System.Drawing
  foreach ($s in 16, 32, 48) {
    try {
      $icon = [Drawing.Icon]::new($Path, $s, $s)
      if ($icon.Width -ne $s) { $problems += "System.Drawing.Icon picked $($icon.Width) for $s" }
      $icon.Dispose()
    } catch { $problems += "System.Drawing.Icon (${s}x$s): $_" }
  }
  $big = $entries | Where-Object Width -eq 256 | Select-Object -First 1
  $small = $entries | Where-Object Width -eq 16 | Select-Object -First 1
  if ($big -and $small -and -not $problems) {
    foreach ($k in $Pixels.Keys) {
      $x, $y = $k -split ',' | ForEach-Object { [int]$_ }
      $p = Get-IcoPixel $big $x $y
      $want = $Pixels[$k]
      if ($want -eq 'transparent') {
        if ($p[0] -gt 16) { $problems += "pixel ($k) is ARGB $($p -join ','), not transparent" }
      } elseif ($p[0] -lt 240 -or [Math]::Abs($p[1] - $want[0]) -gt 24 -or [Math]::Abs($p[2] - $want[1]) -gt 24 -or
                [Math]::Abs($p[3] - $want[2]) -gt 24) {
        $problems += "pixel ($k) is ARGB $($p -join ','), not RGB $($want -join ',')"
      }
    }
    $c = Get-IcoPixel $big 128 128
    $c16 = Get-IcoPixel $small 8 8
    for ($i = 0; $i -lt 4; $i++) {
      if ([Math]::Abs($c[$i] - $c16[$i]) -gt 40) { $problems += "the 16x16 center is ARGB $($c16 -join ','), the 256x256's $($c -join ',')"; break }
    }
  }
  return $problems
}

# A shortcut's System.AppUserModel.ID, read from its property store (the shell
# link object, as the Start menu and the taskbar read it).
function Get-LinkAppUserModelId([string] $Link) {
  if (-not ('GesherLinkProperties' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;

public static class GesherLinkProperties {
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct PropertyKey { public Guid FormatId; public uint PropertyId; }

    [StructLayout(LayoutKind.Sequential)]
    public struct PropVariant { public ushort vt; public ushort r1, r2, r3; public IntPtr p; public IntPtr p2; }

    [ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPropertyStore {
        [PreserveSig] int GetCount(out uint count);
        [PreserveSig] int GetAt(uint index, out PropertyKey key);
        [PreserveSig] int GetValue(ref PropertyKey key, out PropVariant value);
        [PreserveSig] int SetValue(ref PropertyKey key, ref PropVariant value);
        [PreserveSig] int Commit();
    }

    [ComImport, Guid("00021401-0000-0000-C000-000000000046")]
    class ShellLink { }

    [DllImport("ole32.dll")]
    static extern int PropVariantClear(ref PropVariant value);

    public static string AppUserModelId(string path) {
        object link = new ShellLink();
        try {
            ((IPersistFile) link).Load(path, 0);
            var key = new PropertyKey { FormatId = new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"), PropertyId = 5 };
            PropVariant value;
            int hr = ((IPropertyStore) link).GetValue(ref key, out value);
            if (hr != 0) return "(HRESULT 0x" + hr.ToString("x8") + ")";
            try {
                if (value.vt == 31) return Marshal.PtrToStringUni(value.p);  // VT_LPWSTR
                if (value.vt == 8) return Marshal.PtrToStringBSTR(value.p);  // VT_BSTR
                return value.vt == 0 ? "" : "(vt " + value.vt + ")";
            } finally {
                PropVariantClear(ref value);
            }
        } finally {
            Marshal.ReleaseComObject(link);
        }
    }
}
'@
  }
  return [GesherLinkProperties]::AppUserModelId($Link)
}
