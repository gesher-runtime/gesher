# Reading an app's Win32 window from outside its process (docs/graphics.md,
# "An app's window"): PrintWindow (works on hidden windows, no desktop
# needed), the window's title, process, icon and System.AppUserModel.ID, and
# uiapp.apk's (tests/apps/uiapp) expected pixels. Dot-sourced by
# tests/windows/tracks/34-windows.ps1 and by ci.yml's package job.
if (-not ('GesherWin32' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class GesherWin32 {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] struct BITMAPINFOHEADER {
    public uint biSize; public int biWidth, biHeight; public ushort biPlanes, biBitCount;
    public uint biCompression, biSizeImage; public int biXPelsPerMeter, biYPelsPerMeter;
    public uint biClrUsed, biClrImportant; }
  [StructLayout(LayoutKind.Sequential, Pack = 4)] struct PropertyKey { public Guid FormatId; public uint PropertyId; }
  [StructLayout(LayoutKind.Sequential)] struct PropVariant { public ushort vt; public ushort r1, r2, r3; public IntPtr p; public IntPtr p2; }
  [ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IPropertyStore {
    [PreserveSig] int GetCount(out uint count);
    [PreserveSig] int GetAt(uint index, out PropertyKey key);
    [PreserveSig] int GetValue(ref PropertyKey key, out PropVariant value);
    [PreserveSig] int SetValue(ref PropertyKey key, ref PropVariant value);
    [PreserveSig] int Commit();
  }
  [DllImport("shell32.dll")] static extern int SHGetPropertyStoreForWindow(IntPtr hwnd, ref Guid iid,
      [MarshalAs(UnmanagedType.Interface)] out IPropertyStore store);
  [DllImport("ole32.dll")] static extern int PropVariantClear(ref PropVariant value);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hwnd);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref POINT p);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int w, int h, uint flags);
  [DllImport("user32.dll")] public static extern uint MapVirtualKeyW(uint code, uint type);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr hwnd, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr hwnd);
  [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);
  [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr dc);
  [DllImport("gdi32.dll")] static extern bool DeleteDC(IntPtr dc);
  [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc, IntPtr obj);
  [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr obj);
  [DllImport("gdi32.dll")] static extern bool GdiFlush();
  [DllImport("gdi32.dll")] static extern IntPtr CreateDIBSection(IntPtr dc, ref BITMAPINFOHEADER bmi,
      uint usage, out IntPtr bits, IntPtr section, uint offset);
  // The client area's size: {width, height}.
  public static int[] ClientSize(IntPtr hwnd) {
    RECT r;
    GetClientRect(hwnd, out r);
    return new int[] { r.Right - r.Left, r.Bottom - r.Top };
  }
  // Resizes the window so its client area is w x h, as the user dragging its frame does
  // (WM_SIZE; the window's minimum applies), keeping a window that is off the desktop off it.
  public static void SetClientSize(IntPtr hwnd, int w, int h) {
    RECT wr, cr;
    GetWindowRect(hwnd, out wr);
    GetClientRect(hwnd, out cr);
    int ow = w + (wr.Right - wr.Left) - (cr.Right - cr.Left);
    int oh = h + (wr.Bottom - wr.Top) - (cr.Bottom - cr.Top);
    int left = GetSystemMetrics(76 /* SM_XVIRTUALSCREEN */);
    int x = wr.Right <= left ? left - ow - 64 : wr.Left;
    SetWindowPos(hwnd, IntPtr.Zero, x, wr.Top, ow, oh, 0x0004 | 0x0010 /* NOZORDER, NOACTIVATE */);
  }
  // A left click at client (x, y): WM_LBUTTONDOWN, WM_LBUTTONUP.
  public static void Click(IntPtr hwnd, int x, int y) {
    IntPtr at = (IntPtr) ((y << 16) | (x & 0xffff));
    PostMessageW(hwnd, 0x0201, (IntPtr) 1 /* MK_LBUTTON */, at);
    PostMessageW(hwnd, 0x0202, IntPtr.Zero, at);
  }
  // The mouse wheel at client (x, y), `delta` in WHEEL_DELTA units (negative: toward the
  // user, scrolling down). WM_MOUSEWHEEL carries screen coordinates.
  public static void Wheel(IntPtr hwnd, int x, int y, int delta) {
    POINT p = new POINT { X = x, Y = y };
    ClientToScreen(hwnd, ref p);
    IntPtr at = (IntPtr) (((p.Y & 0xffff) << 16) | (p.X & 0xffff));
    PostMessageW(hwnd, 0x020A, (IntPtr) ((delta & 0xffff) << 16), at);
  }
  static void Key(IntPtr hwnd, uint vk, bool down) {
    long scan = MapVirtualKeyW(vk, 0 /* MAPVK_VK_TO_VSC */);
    long l = 1 | (scan << 16) | (down ? 0 : (3L << 30));
    PostMessageW(hwnd, down ? 0x0100u : 0x0101u, (IntPtr) vk, (IntPtr) l);
  }
  // Types letters and digits with WM_KEYDOWN/WM_KEYUP (shift for capitals), as a keyboard
  // does; TranslateMessage adds the WM_CHARs.
  public static void TypeKeys(IntPtr hwnd, string text) {
    foreach (char c in text) {
      bool upper = char.IsUpper(c);
      uint vk = (uint) char.ToUpperInvariant(c);
      if (upper) Key(hwnd, 0x10 /* VK_SHIFT */, true);
      Key(hwnd, vk, true);
      Key(hwnd, vk, false);
      if (upper) Key(hwnd, 0x10, false);
    }
  }
  // WM_CHAR for a character no key types (another keyboard layout, an IME).
  public static void Char(IntPtr hwnd, char c) {
    PostMessageW(hwnd, 0x0102, (IntPtr) c, (IntPtr) 1);
  }
  public static string Title(IntPtr hwnd) {
    var s = new StringBuilder(512);
    GetWindowTextW(hwnd, s, s.Capacity);
    return s.ToString();
  }
  public static uint ProcessId(IntPtr hwnd) {
    uint pid;
    GetWindowThreadProcessId(hwnd, out pid);
    return pid;
  }
  // WM_GETICON ICON_BIG: non-zero once the window was given an icon (WM_SETICON).
  public static bool HasIcon(IntPtr hwnd) {
    return SendMessageW(hwnd, 0x007F, (IntPtr) 1, IntPtr.Zero) != IntPtr.Zero;
  }
  // The window's System.AppUserModel.ID (what the taskbar groups it by), or "".
  public static string AppUserModelId(IntPtr hwnd) {
    Guid iid = new Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99");
    IPropertyStore store;
    int hr = SHGetPropertyStoreForWindow(hwnd, ref iid, out store);
    if (hr != 0) return "(HRESULT 0x" + hr.ToString("x8") + ")";
    try {
      var key = new PropertyKey { FormatId = new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"), PropertyId = 5 };
      PropVariant value;
      hr = store.GetValue(ref key, out value);
      if (hr != 0) return "(HRESULT 0x" + hr.ToString("x8") + ")";
      try {
        if (value.vt == 31) return Marshal.PtrToStringUni(value.p);  // VT_LPWSTR
        return value.vt == 0 ? "" : "(vt " + value.vt + ")";
      } finally {
        PropVariantClear(ref value);
      }
    } finally {
      Marshal.ReleaseComObject(store);
    }
  }
  // The client area as 0xRRGGBB values row by row after [width, height]; null on failure.
  public static int[] Capture(IntPtr hwnd) {
    RECT r;
    if (!GetClientRect(hwnd, out r)) return null;
    int w = r.Right - r.Left, h = r.Bottom - r.Top;
    if (w < 1 || h < 1) return null;
    IntPtr screen = GetDC(IntPtr.Zero), dc = CreateCompatibleDC(screen);
    var bmi = new BITMAPINFOHEADER { biSize = 40, biWidth = w, biHeight = -h, biPlanes = 1, biBitCount = 32 };
    IntPtr bits;
    IntPtr bmp = CreateDIBSection(dc, ref bmi, 0, out bits, IntPtr.Zero, 0);
    int[] pixels = null;
    if (bmp != IntPtr.Zero) {
      IntPtr old = SelectObject(dc, bmp);
      if (PrintWindow(hwnd, dc, 1 /* PW_CLIENTONLY */)) {
        GdiFlush();
        pixels = new int[w * h + 2];
        pixels[0] = w; pixels[1] = h;
        Marshal.Copy(bits, pixels, 2, w * h);
        for (int i = 2; i < pixels.Length; i++) pixels[i] &= 0xffffff;
      }
      SelectObject(dc, old);
      DeleteObject(bmp);
    }
    DeleteDC(dc);
    ReleaseDC(IntPtr.Zero, screen);
    return pixels;
  }
  // Pixels within `tolerance` of 0xRRGGBB `want` in [x0, x1) x [y0, y1).
  public static int Count(int[] px, int x0, int y0, int x1, int y1, int want, int tolerance) {
    int w = px[0], h = px[1], n = 0;
    for (int y = Math.Max(0, y0); y < Math.Min(h, y1); y++)
      for (int x = Math.Max(0, x0); x < Math.Min(w, x1); x++) {
        int c = px[2 + y * w + x];
        if (Math.Abs(((c >> 16) & 255) - ((want >> 16) & 255)) <= tolerance &&
            Math.Abs(((c >> 8) & 255) - ((want >> 8) & 255)) <= tolerance &&
            Math.Abs((c & 255) - (want & 255)) <= tolerance) n++;
      }
    return n;
  }
}
'@
}

# uiapp's layout at 400x800 and 160 dpi (1 dp = 1 px): red 0-200, a green
# Button and a blue View 200-400, a yellow TextView 400-520 with black text, a
# white EditText 520-600, a gray TextView 600-680 (its echo), a ScrollView
# showing magenta 680-800.
function UiAppPixels($px) {
  $w = $px[0]; $h = $px[1]
  $out = @()
  if ($w -ne 400 -or $h -ne 800) { $out += "client area $($w)x$($h), not 400x800" }
  $out += @(Regions $px @(@('red', 0, 0, 400, 200, 0xff0000), @('green', 0, 200, 200, 400, 0x00ff00),
                           @('blue', 200, 200, 400, 400, 0x0000ff), @('EditText', 0, 520, 400, 600, 0xffffff, 0.95),
                           @('echo', 0, 600, 400, 680, 0xc0c0c0), @('magenta', 0, 680, 400, 800, 0xff00ff)))
  $yellow = [GesherWin32]::Count($px, 0, 400, 400, 520, 0xffff00, 40)
  $dark = [GesherWin32]::Count($px, 0, 400, 400, 520, 0x000000, 100)
  Write-Host "INFO  TextView: $yellow yellow and $dark dark (text) pixels of 48000"
  if ($yellow -lt 30000) { $out += "TextView background: $yellow yellow pixels" }
  if ($dark -lt 100) { $out += "TextView text: $dark dark pixels" }
  return $out
}

# Problems with regions of $px: each @(name, x0, y0, x1, y1, 0xRRGGBB[, share])
# must be at least `share` (0.98) that color.
function Regions($px, $regions) {
  $out = @()
  foreach ($r in $regions) {
    $share = if ($r.Count -gt 6) { $r[6] } else { 0.98 }
    $area = ($r[3] - $r[1]) * ($r[4] - $r[2])
    $n = [GesherWin32]::Count($px, $r[1], $r[2], $r[3], $r[4], $r[5], 40)
    Write-Host "INFO  $($r[0]): $n of $area pixels"
    if ($n -lt $area * $share) { $out += "$($r[0]): $n of $area pixels" }
  }
  return $out
}

# Waits up to $Seconds for $Want in what $ReadLog returns.
function WaitForLog([scriptblock] $ReadLog, [string] $Want, [int] $Seconds = 90) {
  $deadline = (Get-Date).AddSeconds($Seconds)
  do {
    if ("$(& $ReadLog)".Contains($Want)) { return $true }
    Start-Sleep -Milliseconds 250
  } while ((Get-Date) -lt $deadline)
  return $false
}

# Input reaches uiapp's views (docs/input.md, docs/graphics.md): Win32 messages
# posted to its (off-desktop) window, the app's log ($ReadLog) and the window
# read back with PrintWindow. A click on the Button (its onClick turns the red
# View cyan); keys typing "Gesher" into the focused EditText, then a WM_CHAR
# no key types (U+05D0), echoed into the gray TextView; the mouse wheel over
# the ScrollView; the user resizing the window (below its minimum, then to
# 600x700), which lays the views out and draws them at the new size.
# Returns the problems.
function UiAppInput([IntPtr] $hwnd, [scriptblock] $ReadLog) {
  $out = @()
  [GesherWin32]::Click($hwnd, 100, 300)
  if (-not (WaitForLog $ReadLog 'UiActivity onClick: button')) { $out += 'no onClick' } else {
    Start-Sleep 2
    $out += @(Regions ([GesherWin32]::Capture($hwnd)) @(, @('cyan after the click', 0, 0, 400, 200, 0x00ffff)))
  }
  [GesherWin32]::TypeKeys($hwnd, 'Gesher')
  if (-not (WaitForLog $ReadLog 'UiActivity echo: Gesher')) {
    $m = [regex]::Matches("$(& $ReadLog)", 'UiActivity echo: [^\r\n]*')
    $out += "no 'UiActivity echo: Gesher' (last: '$(if ($m.Count) { $m[$m.Count - 1].Value })')"
  } else {
    [GesherWin32]::Char($hwnd, [char]0x05D0)
    if (-not (WaitForLog $ReadLog 'UiActivity echo: Gesher\u05d0' 30)) { $out += 'WM_CHAR U+05D0 not echoed' }
    Start-Sleep 2
    $px = [GesherWin32]::Capture($hwnd)
    $dark = [GesherWin32]::Count($px, 0, 600, 400, 680, 0x000000, 100)
    $edit = [GesherWin32]::Count($px, 0, 520, 400, 600, 0x000000, 100)
    Write-Host "INFO  typed text: $edit dark pixels in the EditText, $dark in its echo"
    if ($dark -lt 100) { $out += "echo: $dark dark pixels" }
    if ($edit -lt 100) { $out += "EditText: $edit dark pixels" }
  }
  [GesherWin32]::Wheel($hwnd, 200, 740, -360)
  if (-not (WaitForLog $ReadLog 'UiActivity scrolled to ' 60)) { $out += 'the wheel did not scroll' } else {
    Start-Sleep 2
    $px = [GesherWin32]::Capture($hwnd)
    $magenta = [GesherWin32]::Count($px, 0, 680, 400, 800, 0xff00ff, 40)
    $other = [GesherWin32]::Count($px, 0, 680, 400, 800, 0xff8000, 40) + [GesherWin32]::Count($px, 0, 680, 400, 800, 0x800080, 40)
    Write-Host "INFO  scrolled: $magenta magenta, $other orange and purple pixels of 48000"
    if ($magenta -gt 24000 -or $other -lt 24000) { $out += "scrolled: $magenta magenta, $other orange and purple pixels" }
  }
  # Smaller than the window's minimum (220dp: 220 px at 160 dpi): kept at it.
  [GesherWin32]::SetClientSize($hwnd, 100, 100)
  $min = [GesherWin32]::ClientSize($hwnd)
  Write-Host "INFO  resized to 100x100: the client area is $($min[0])x$($min[1])"
  if ($min[0] -lt 220 -or $min[1] -lt 220) { $out += "resized below the minimum: $($min[0])x$($min[1])" }
  elseif (-not (WaitForLog $ReadLog "UiActivity content $($min[0])x$($min[1])" 60)) { $out += "no layout at $($min[0])x$($min[1])" }
  [GesherWin32]::SetClientSize($hwnd, 600, 700)
  if (-not (WaitForLog $ReadLog 'UiActivity content 600x700' 60)) { $out += 'no layout at 600x700' } else {
    Start-Sleep 3
    $px = [GesherWin32]::Capture($hwnd)
    if ($px[0] -ne 600 -or $px[1] -ne 700) { $out += "client area $($px[0])x$($px[1]) after resizing to 600x700" }
    else {
      $out += @(Regions $px @(@('cyan at 600x700', 0, 0, 600, 200, 0x00ffff), @('green at 600x700', 0, 200, 300, 400, 0x00ff00),
                              @('blue at 600x700', 300, 200, 600, 400, 0x0000ff)))
    }
  }
  return $out
}
