// Temporary CI probe (ime track): what the runners' IMEs and keyboard input do.
using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Text;

public static class ImeProbe {
  delegate IntPtr WndProc(IntPtr h, uint m, IntPtr w, IntPtr l);
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  struct WNDCLASS { public uint style; public WndProc proc; public int c1, c2; public IntPtr inst, icon, cursor, bg; public string menu, cls; }
  [StructLayout(LayoutKind.Sequential)] struct MSG { public IntPtr h; public uint m; public IntPtr w, l; public uint t; public int x, y; }
  [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort vk, scan; public uint flags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Explicit)] struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; [FieldOffset(8)] public long pad0; [FieldOffset(16)] public long pad1; [FieldOffset(24)] public long pad2; [FieldOffset(32)] public long pad3; }
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern ushort RegisterClassW(ref WNDCLASS c);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr CreateWindowExW(uint ex, string cls, string name, uint style, int x, int y, int w, int h, IntPtr p, IntPtr menu, IntPtr inst, IntPtr param);
  [DllImport("user32.dll")] static extern IntPtr DefWindowProcW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] static extern int GetMessageW(out MSG m, IntPtr h, uint a, uint b);
  [DllImport("user32.dll")] static extern bool TranslateMessage(ref MSG m);
  [DllImport("user32.dll")] static extern IntPtr DispatchMessageW(ref MSG m);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr LoadKeyboardLayoutW(string id, uint flags);
  [DllImport("user32.dll")] static extern IntPtr GetKeyboardLayout(uint thread);
  [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
  [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
  [DllImport("user32.dll")] static extern uint MapVirtualKeyW(uint code, uint type);
  [DllImport("user32.dll")] static extern int GetKeyboardLayoutList(int n, IntPtr[] list);
  [DllImport("user32.dll")] static extern IntPtr GetFocus();
  [DllImport("user32.dll")] static extern bool CreateCaret(IntPtr h, IntPtr bmp, int w, int hgt);
  [DllImport("user32.dll")] static extern bool SetCaretPos(int x, int y);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] struct COMPOSITIONFORM { public uint style; public POINT pt; public RECT area; }
  [StructLayout(LayoutKind.Sequential)] struct CANDIDATEFORM { public uint index, style; public POINT pt; public RECT area; }
  [StructLayout(LayoutKind.Sequential)] public struct GUITHREADINFO { public int cbSize; public uint flags; public IntPtr active, focus, capture, menuOwner, moveSize, caret; public RECT rcCaret; }
  [DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint tid, ref GUITHREADINFO info);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("imm32.dll")] static extern bool ImmSetCompositionWindow(IntPtr c, ref COMPOSITIONFORM f);
  [DllImport("imm32.dll")] static extern bool ImmSetCandidateWindow(IntPtr c, ref CANDIDATEFORM f);
  [DllImport("imm32.dll")] static extern IntPtr ImmGetContext(IntPtr h);
  [DllImport("imm32.dll")] static extern bool ImmReleaseContext(IntPtr h, IntPtr c);
  [DllImport("imm32.dll", CharSet = CharSet.Unicode)] static extern bool ImmSetCompositionStringW(IntPtr c, uint index, string comp, int len, string read, int rlen);
  [DllImport("imm32.dll", CharSet = CharSet.Unicode)] static extern int ImmGetCompositionStringW(IntPtr c, uint index, byte[] buf, int len);
  [DllImport("imm32.dll")] static extern bool ImmIsIME(IntPtr hkl);
  [DllImport("imm32.dll")] static extern bool ImmGetOpenStatus(IntPtr c);
  [DllImport("imm32.dll")] static extern bool ImmSetOpenStatus(IntPtr c, bool o);
  [DllImport("imm32.dll")] static extern bool ImmSetConversionStatus(IntPtr c, uint conv, uint sent);
  [DllImport("imm32.dll")] static extern bool ImmGetConversionStatus(IntPtr c, out uint conv, out uint sent);
  [DllImport("imm32.dll")] static extern bool ImmNotifyIME(IntPtr c, uint a, uint i, uint v);

  [ComImport, Guid("71C6E74C-0F28-11D8-A82A-00065B84435C"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface ITfInputProcessorProfileMgr {
    [PreserveSig] int ActivateProfile(uint type, ushort langid, ref Guid clsid, ref Guid profile, IntPtr hkl, uint flags);
  }
  [ComImport, Guid("33C53A50-F456-4884-B049-85FD643ECFED")] class TfProfiles {}

  static WndProc proc = Proc;
  public static IntPtr Hwnd;
  static StringBuilder log = new StringBuilder();
  static void L(string s) { lock (log) log.AppendLine("  " + s); }
  public static string Log() { lock (log) { var s = log.ToString(); log.Clear(); return s; } }
  public static string Tip = "";

  static string Comp(IntPtr c, uint idx) {
    int n = ImmGetCompositionStringW(c, idx, null, 0);
    if (n <= 0) return "(" + n + ")";
    var b = new byte[n]; ImmGetCompositionStringW(c, idx, b, n);
    return Encoding.Unicode.GetString(b);
  }

  static IntPtr Proc(IntPtr h, uint m, IntPtr w, IntPtr l) {
    switch (m) {
      case 0x0100: L("WM_KEYDOWN " + w.ToInt64().ToString("x")); break;
      case 0x0102: L("WM_CHAR U+" + w.ToInt64().ToString("x4")); break;
      case 0x0103: L("WM_DEADCHAR U+" + w.ToInt64().ToString("x4")); break;
      case 0x0051: L("WM_INPUTLANGCHANGE " + l.ToInt64().ToString("x")); break;
      case 0x010D: L("WM_IME_STARTCOMPOSITION"); break;
      case 0x010E: L("WM_IME_ENDCOMPOSITION"); break;
      case 0x010F: {
        IntPtr c = ImmGetContext(h);
        L("WM_IME_COMPOSITION flags " + l.ToInt64().ToString("x") + " comp '" + Comp(c, 8) + "' result '" + Comp(c, 0x800) + "'");
        ImmReleaseContext(h, c);
        break; }
      case 0x0282: L("WM_IME_NOTIFY " + w.ToInt64().ToString("x")); break;
      case 0x0286: L("WM_IME_CHAR U+" + w.ToInt64().ToString("x4")); break;
      case 0x0400 + 1: {
        IntPtr c = ImmGetContext(h);
        IntPtr hkl = GetKeyboardLayout(0);
        uint conv, sent; ImmGetConversionStatus(c, out conv, out sent);
        L("layout " + hkl.ToInt64().ToString("x") + " isIME " + ImmIsIME(hkl) + " open " + ImmGetOpenStatus(c) + " conv " + conv.ToString("x"));
        ImmReleaseContext(h, c);
        return IntPtr.Zero; }
      case 0x0400 + 2: {
        IntPtr c = ImmGetContext(h);
        L("SetOpenStatus: " + ImmSetOpenStatus(c, true) + " open " + ImmGetOpenStatus(c));
        if (w != IntPtr.Zero) L("SetConversionStatus: " + ImmSetConversionStatus(c, (uint) w.ToInt64(), 0));
        ImmReleaseContext(h, c);
        return IntPtr.Zero; }
      case 0x0400 + 5: {
        L("CreateCaret " + CreateCaret(h, IntPtr.Zero, 2, 30) + " SetCaretPos " + SetCaretPos(150, 60));
        IntPtr c = ImmGetContext(h);
        var cf = new COMPOSITIONFORM { style = 2 /* CFS_POINT */, pt = new POINT { X = 150, Y = 60 } };
        var cand = new CANDIDATEFORM { index = 0, style = 0x80 /* CFS_EXCLUDE */, pt = new POINT { X = 150, Y = 90 },
                                       area = new RECT { Left = 150, Top = 60, Right = 152, Bottom = 90 } };
        L("ImmSetCompositionWindow " + ImmSetCompositionWindow(c, ref cf) + " ImmSetCandidateWindow " + ImmSetCandidateWindow(c, ref cand));
        ImmReleaseContext(h, c);
        return IntPtr.Zero; }
      case 0x0400 + 3: {
        IntPtr c = ImmGetContext(h);
        bool ok = ImmSetCompositionStringW(c, 9 /* SCS_SETSTR */, "nihon", 10, null, 0);
        L("ImmSetCompositionStringW SETSTR: " + ok);
        L("ImmNotifyIME complete: " + ImmNotifyIME(c, 0x15, 1, 0));
        ImmReleaseContext(h, c);
        return IntPtr.Zero; }
      case 0x0400 + 4: {
        try {
          var mgr = (ITfInputProcessorProfileMgr) new TfProfiles();
          Guid clsid, prof; ushort lang;
          if (Tip == "ja") { clsid = new Guid("03B5835F-F03C-411B-9CE2-AA23E1171E36"); prof = new Guid("A76C93D9-5523-4E90-AAFA-4DB112F9AC76"); lang = 0x411; }
          else if (Tip == "ko") { clsid = new Guid("A028AE76-01B1-46C2-99C4-ACD9858AE02F"); prof = new Guid("B5FE1F02-D5F2-4445-9C03-C568F23C99A1"); lang = 0x412; }
          else { clsid = new Guid("81D4E9C9-1D3B-41BC-9E6C-4B40BF79E35E"); prof = new Guid("FA550B04-5AD7-411F-A5AC-CA038EC515D7"); lang = 0x804; }
          int hr = mgr.ActivateProfile(1, lang, ref clsid, ref prof, IntPtr.Zero, 0x10000000 | 4 | 1);
          L("ActivateProfile " + Tip + ": 0x" + hr.ToString("x8") + " layout " + GetKeyboardLayout(0).ToInt64().ToString("x"));
        } catch (Exception e) { L("ActivateProfile: " + e.Message); }
        return IntPtr.Zero; }
    }
    return DefWindowProcW(h, m, w, l);
  }

  public static void Start() {
    var ready = new ManualResetEvent(false);
    var t = new Thread(() => {
      var wc = new WNDCLASS { proc = proc, cls = "ImeProbe" };
      RegisterClassW(ref wc);
      Hwnd = CreateWindowExW(0, "ImeProbe", "ime probe", 0x10CF0000, 100, 100, 300, 200, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
      ready.Set();
      MSG msg;
      while (GetMessageW(out msg, IntPtr.Zero, 0, 0) > 0) { TranslateMessage(ref msg); DispatchMessageW(ref msg); }
    });
    t.IsBackground = true;
    t.SetApartmentState(ApartmentState.STA);
    t.Start();
    ready.WaitOne();
  }

  public static bool Foreground(IntPtr h) {
    for (int i = 0; i < 5 && GetForegroundWindow() != h; i++) {
      keybd_event(0x12, 0, 0, IntPtr.Zero);  // Alt: lets this process set the foreground
      keybd_event(0x12, 0, 2, IntPtr.Zero);
      SetForegroundWindow(h);
      Thread.Sleep(200);
    }
    return GetForegroundWindow() == h;
  }

  static INPUT Kbd(ushort vk, ushort scan, uint flags) {
    var i = new INPUT { type = 1 };
    i.ki.vk = vk; i.ki.scan = scan; i.ki.flags = flags;
    return i;
  }

  public static uint Unicode(string s) {
    var list = new System.Collections.Generic.List<INPUT>();
    foreach (char c in s) { list.Add(Kbd(0, c, 4)); list.Add(Kbd(0, c, 4 | 2)); }
    return SendInput((uint) list.Count, list.ToArray(), Marshal.SizeOf(typeof(INPUT)));
  }

  // Virtual keys, pressed and released one after the other.
  public static uint Keys(params ushort[] vks) {
    var list = new System.Collections.Generic.List<INPUT>();
    foreach (ushort vk in vks) {
      ushort scan = (ushort) MapVirtualKeyW(vk, 0);
      list.Add(Kbd(vk, scan, 0)); list.Add(Kbd(vk, scan, 2));
    }
    return SendInput((uint) list.Count, list.ToArray(), Marshal.SizeOf(typeof(INPUT)));
  }

  public static string Windows() {
    uint me = (uint) System.Diagnostics.Process.GetCurrentProcess().Id;
    var s = new StringBuilder();
    EnumWindows((h, l) => {
      uint pid; GetWindowThreadProcessId(h, out pid);
      var cls = new StringBuilder(256); GetClassNameW(h, cls, 256);
      if (IsWindowVisible(h)) {
        RECT r; GetWindowRect(h, out r);
        string c = cls.ToString();
        if (pid == me || c.Contains("IME") || c.Contains("Cand") || c.Contains("IPTip") || c.Contains("Input") || c.Contains("Core"))
          s.Append("  window " + c + " pid " + pid + (pid == me ? " (this)" : "") + " at " + r.Left + "," + r.Top + " " + (r.Right - r.Left) + "x" + (r.Bottom - r.Top) + "\n");
      }
      return true;
    }, IntPtr.Zero);
    return s.ToString();
  }

  public static string Caret() {
    uint pid; uint tid = GetWindowThreadProcessId(Hwnd, out pid);
    var g = new GUITHREADINFO { cbSize = Marshal.SizeOf(typeof(GUITHREADINFO)) };
    bool ok = GetGUIThreadInfo(tid, ref g);
    var p = new POINT { X = g.rcCaret.Left, Y = g.rcCaret.Top };
    ClientToScreen(g.caret, ref p);
    return "caret " + ok + " hwnd " + g.caret + " rc " + g.rcCaret.Left + "," + g.rcCaret.Top + "-" + g.rcCaret.Right + "," + g.rcCaret.Bottom + " screen " + p.X + "," + p.Y;
  }

  public static int ActivateHere(string tip) {
    var mgr = (ITfInputProcessorProfileMgr) new TfProfiles();
    Guid clsid, prof; ushort lang;
    if (tip == "ja") { clsid = new Guid("03B5835F-F03C-411B-9CE2-AA23E1171E36"); prof = new Guid("A76C93D9-5523-4E90-AAFA-4DB112F9AC76"); lang = 0x411; }
    else { clsid = new Guid("A028AE76-01B1-46C2-99C4-ACD9858AE02F"); prof = new Guid("B5FE1F02-D5F2-4445-9C03-C568F23C99A1"); lang = 0x412; }
    return mgr.ActivateProfile(1, lang, ref clsid, ref prof, IntPtr.Zero, 1 | 4);
  }

  public static string Layouts() {
    var a = new IntPtr[64];
    int n = GetKeyboardLayoutList(64, a);
    var s = new StringBuilder();
    for (int i = 0; i < n; i++) s.Append(a[i].ToInt64().ToString("x")).Append(' ');
    return s.ToString();
  }
}
