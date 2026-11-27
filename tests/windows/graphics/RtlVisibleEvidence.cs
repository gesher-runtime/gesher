// Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
using System;
using System.Runtime.InteropServices;
using System.Text;

// Reads only a bounded patch of the synthetic fixture's actual desktop client.
// No desktop bitmap or other raw pixels are persisted or emitted by the caller.
public sealed class RtlVisibleEvidence : IDisposable {
    [StructLayout(LayoutKind.Sequential)]
    struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)]
    struct Point { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)]
    struct Monitor { public int Size; public Rect Bounds, Work; public int Flags; }
    [StructLayout(LayoutKind.Sequential)]
    struct Dib {
        public uint Size;
        public int Width, Height;
        public ushort Planes, Depth;
        public uint Compression, ImageSize, XDensity, YDensity, Colors, Required;
    }
    [DllImport("user32.dll")] static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
    [DllImport("user32.dll")] static extern bool CloseDesktop(IntPtr desktop);
    [DllImport("user32.dll")] static extern IntPtr GetThreadDesktop(uint id);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    static extern bool GetUserObjectInformationW(IntPtr obj, int index,
        StringBuilder value, int bytes, out int needed);
    [DllImport("wtsapi32.dll")] static extern bool WTSQuerySessionInformationW(IntPtr server, int session,
        int kind, out IntPtr buffer, out int bytes);
    [DllImport("wtsapi32.dll")] static extern void WTSFreeMemory(IntPtr buffer);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll")] static extern bool GetProcessTimes(IntPtr process, out long created,
        out long exited, out long kernel, out long user);
    [DllImport("advapi32.dll")] static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll")] static extern bool GetTokenInformation(IntPtr token, int kind, out uint value,
        int size, out int needed);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] static extern bool PostMessageW(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll")] static extern IntPtr GetTopWindow(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr window, uint command);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr window, out Rect bounds);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr window, ref Point point);
    [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr value);
    [DllImport("user32.dll")] static extern IntPtr MonitorFromWindow(IntPtr window, uint flags);
    [DllImport("user32.dll")] static extern bool GetMonitorInfoW(IntPtr monitor, ref Monitor value);
    [DllImport("user32.dll")] static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr window, int attribute, out Rect value, int size);
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr window, int attribute, out uint value, int size);
    [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr window);
    [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr window, IntPtr dc);
    [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr dc);
    [DllImport("gdi32.dll")] static extern bool DeleteDC(IntPtr dc);
    [DllImport("gdi32.dll")] static extern IntPtr CreateDIBSection(IntPtr dc, ref Dib dib, int usage, out IntPtr bits,
        IntPtr section, uint offset);
    [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc, IntPtr obj);
    [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr obj);
    [DllImport("gdi32.dll")] static extern bool GdiFlush();
    [DllImport("gdi32.dll")] static extern bool BitBlt(IntPtr dest, int x, int y, int w, int h,
        IntPtr source, int sx, int sy, uint operation);

    const int Width = 400;
    readonly IntPtr window;
    readonly uint pid, thread;
    readonly long birth;
    IntPtr process;

    public RtlVisibleEvidence(IntPtr window, uint pid, long notBefore) {
        this.window = window;
        this.pid = pid;
        uint actual;
        thread = GetWindowThreadProcessId(window, out actual);
        Require(thread != 0 && pid != 0 && actual == pid, "fixture HWND does not match the reported app PID");
        process = OpenProcess(0x101000, false, pid); // SYNCHRONIZE | QUERY_LIMITED_INFORMATION
        Require(process != IntPtr.Zero, "Windows denied the fixture process query");
        try {
            long unused;
            Require(GetProcessTimes(process, out birth, out unused, out unused, out unused) && birth >= notBefore,
                "fixture PID was not created by the current run");
            IntPtr token;
            Require(OpenProcessToken(process, 8, out token), "Windows denied the fixture token query");
            try {
                uint contained;
                int needed;
                Require(GetTokenInformation(token, 29, out contained, sizeof(uint), out needed) &&
                    needed == sizeof(uint) && contained == 1, "fixture HWND owner is not an AppContainer");
            } finally { CloseHandle(token); }
            VerifyOwner();
        } catch { Dispose(); throw; }
    }

    static void Require(bool ok, string error) {
        if (!ok) throw new InvalidOperationException(error);
    }
    static string DesktopName(IntPtr desktop) {
        var name = new StringBuilder(256);
        int needed;
        return desktop != IntPtr.Zero && GetUserObjectInformationW(desktop, 2, name, name.Capacity * 2, out needed)
            ? name.ToString() : "";
    }
    public static void VerifyInputDesktop() {
        IntPtr state;
        int bytes;
        Require(WTSQuerySessionInformationW(IntPtr.Zero, -1, 8, out state, out bytes), "WTS session unavailable");
        try { Require(bytes == sizeof(int) && Marshal.ReadInt32(state) == 0, "WTS session is not active"); }
        finally { WTSFreeMemory(state); }
        IntPtr input = OpenInputDesktop(0, false, 1); // DESKTOP_READOBJECTS
        Require(input != IntPtr.Zero, "Windows denied the input desktop query");
        try {
            Require(String.Equals(DesktopName(input), "Default", StringComparison.OrdinalIgnoreCase) &&
                String.Equals(DesktopName(GetThreadDesktop(GetCurrentThreadId())), "Default", StringComparison.OrdinalIgnoreCase),
                "the current thread is not on the active Default desktop");
        } finally { CloseDesktop(input); }
    }
    void VerifyOwner() {
        uint actual;
        Require(process != IntPtr.Zero && WaitForSingleObject(process, 0) == 258 &&
            GetWindowThreadProcessId(window, out actual) == thread && actual == pid,
            "the original fixture process no longer owns the HWND");
    }
    public bool TryForeground() {
        VerifyOwner();
        VerifyInputDesktop();
        return GetForegroundWindow() == window || (SetForegroundWindow(window) && GetForegroundWindow() == window);
    }
    Rect VerifyPatch(int top, int height) {
        VerifyOwner();
        VerifyInputDesktop();
        Require(GetForegroundWindow() == window && IsWindowVisible(window) && !IsIconic(window),
            "fixture is not visible in the foreground");
        Rect client;
        var origin = new Point { Y = top };
        Require(GetClientRect(window, out client) && client.Right == Width && top + height <= client.Bottom &&
            ClientToScreen(window, ref origin), "fixture client does not cover the requested physical patch");
        var patch = new Rect { Left = origin.X, Right = origin.X + Width, Top = origin.Y, Bottom = origin.Y + height };
        var monitor = new Monitor { Size = Marshal.SizeOf(typeof(Monitor)) };
        Require(GetMonitorInfoW(MonitorFromWindow(window, 0), ref monitor) && patch.Left >= monitor.Bounds.Left &&
            patch.Right <= monitor.Bounds.Right && patch.Top >= monitor.Bounds.Top && patch.Bottom <= monitor.Bounds.Bottom,
            "fixture patch is clipped by the physical monitor");
        IntPtr above = GetTopWindow(IntPtr.Zero);
        for (int scanned = 0; scanned < 512 && above != IntPtr.Zero; ++scanned, above = GetWindow(above, 2)) {
            if (above == window) return patch;
            if (!IsWindowVisible(above) || IsIconic(above)) continue;
            uint cloaked;
            if (DwmGetWindowAttribute(above, 14, out cloaked, sizeof(uint)) == 0 && cloaked != 0) continue;
            Rect bounds;
            Require(GetWindowRect(above, out bounds), "could not verify desktop window coverage");
            Require(bounds.Left >= patch.Right || bounds.Right <= patch.Left || bounds.Top >= patch.Bottom ||
                bounds.Bottom <= patch.Top, "another window overlaps the fixture patch");
        }
        throw new InvalidOperationException("fixture not found in bounded desktop z-order");
    }
    public int[] Capture(int top, int height) {
        Require(top >= 0 && top <= 800 && height > 0 && height <= 160, "invalid fixture patch");
        IntPtr dpi = SetThreadDpiAwarenessContext(new IntPtr(-4)); // per-monitor aware v2, physical pixels
        Require(dpi != IntPtr.Zero, "per-monitor DPI awareness unavailable");
        IntPtr screen = IntPtr.Zero, dc = IntPtr.Zero, bitmap = IntPtr.Zero, previous = IntPtr.Zero;
        try {
            Rect patch = VerifyPatch(top, height);
            screen = GetDC(IntPtr.Zero);
            Require(screen != IntPtr.Zero, "no desktop display DC");
            dc = CreateCompatibleDC(screen);
            Require(dc != IntPtr.Zero, "could not allocate fixture-patch DC");
            var dib = new Dib { Size = (uint)Marshal.SizeOf(typeof(Dib)), Width = Width, Height = -height, Planes = 1, Depth = 32 };
            IntPtr bits;
            bitmap = CreateDIBSection(dc, ref dib, 0, out bits, IntPtr.Zero, 0);
            Require(bitmap != IntPtr.Zero && bits != IntPtr.Zero, "could not allocate fixture-patch bitmap");
            previous = SelectObject(dc, bitmap);
            Require(previous != IntPtr.Zero && previous != new IntPtr(-1), "could not select the fixture-patch bitmap");
            // SRCCOPY | CAPTUREBLT | NOMIRRORBITMAP: sample the displayed screen in physical order.
            Require(BitBlt(dc, 0, 0, Width, height, screen, patch.Left, patch.Top, 0xc0cc0020) && GdiFlush(),
                "desktop screen BitBlt failed");
            Rect after = VerifyPatch(top, height);
            Require(after.Left == patch.Left && after.Top == patch.Top, "fixture moved during desktop acquisition");
            int[] pixels = new int[2 + Width * height];
            pixels[0] = Width; pixels[1] = height;
            Marshal.Copy(bits, pixels, 2, Width * height);
            return pixels;
        } finally {
            if (previous != IntPtr.Zero && previous != new IntPtr(-1)) SelectObject(dc, previous);
            if (bitmap != IntPtr.Zero) DeleteObject(bitmap);
            if (dc != IntPtr.Zero) DeleteDC(dc);
            if (screen != IntPtr.Zero) ReleaseDC(IntPtr.Zero, screen);
            SetThreadDpiAwarenessContext(dpi);
        }
    }
    public string Describe() {
        VerifyOwner();
        string caption = "unavailable";
        IntPtr dpi = SetThreadDpiAwarenessContext(new IntPtr(-4));
        if (dpi != IntPtr.Zero) {
            try {
                Rect frame, buttons;
                int size = Marshal.SizeOf(typeof(Rect));
                if (GetForegroundWindow() == window && IsWindowVisible(window) && !IsIconic(window) &&
                    GetWindowRect(window, out frame) && DwmGetWindowAttribute(window, 5, out buttons, size) == 0 &&
                    GetForegroundWindow() == window && IsWindowVisible(window) && !IsIconic(window) &&
                    buttons.Left >= 0 && buttons.Right > buttons.Left && buttons.Right <= frame.Right - frame.Left)
                    caption = 2 * buttons.Right < frame.Right - frame.Left ? "left" : "right or centered";
            } finally { SetThreadDpiAwarenessContext(dpi); }
        }
        VerifyOwner();
        return "pid=" + pid + " birth=" + birth + " AppContainer=1 dpi=" + GetDpiForWindow(window) +
            " DWM caption buttons=" + caption;
    }
    public void CloseFixture() {
        VerifyOwner();
        Require(PostMessageW(window, 0x10, IntPtr.Zero, IntPtr.Zero), "fixture WM_CLOSE failed");
    }
    public void Dispose() {
        if (process != IntPtr.Zero) { CloseHandle(process); process = IntPtr.Zero; }
    }
}
