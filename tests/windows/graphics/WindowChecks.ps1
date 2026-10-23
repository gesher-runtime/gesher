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

# uiapp's layout at 400x800 and 160 dpi (1 dp = 1 px): red 0-200, green and
# blue halves 200-400, a yellow TextView 400-520 with black text, white below.
function UiAppPixels($px) {
  $w = $px[0]; $h = $px[1]
  $out = @()
  if ($w -ne 400 -or $h -ne 800) { $out += "client area $($w)x$($h), not 400x800" }
  foreach ($r in @(@('red', 0, 0, 400, 200, 0xff0000), @('green', 0, 200, 200, 400, 0x00ff00),
                   @('blue', 200, 200, 400, 400, 0x0000ff), @('white', 0, 520, 400, 800, 0xffffff))) {
    $area = ($r[3] - $r[1]) * ($r[4] - $r[2])
    $n = [GesherWin32]::Count($px, $r[1], $r[2], $r[3], $r[4], $r[5], 40)
    Write-Host "INFO  $($r[0]): $n of $area pixels"
    if ($n -lt $area * 0.98) { $out += "$($r[0]): $n of $area pixels" }
  }
  $yellow = [GesherWin32]::Count($px, 0, 400, 400, 520, 0xffff00, 40)
  $dark = [GesherWin32]::Count($px, 0, 400, 400, 520, 0x000000, 100)
  Write-Host "INFO  TextView: $yellow yellow and $dark dark (text) pixels of 48000"
  if ($yellow -lt 30000) { $out += "TextView background: $yellow yellow pixels" }
  if ($dark -lt 100) { $out += "TextView text: $dark dark pixels" }
  return $out
}

