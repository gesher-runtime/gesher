// Real keyboard input for a Gesher window (tests/windows/tracks/35-ime.ps1,
// docs/input.md "Text input"): the window brought to the foreground,
// SendInput's Unicode text and virtual keys, its thread's keyboard layout
// switched (WM_INPUTLANGCHANGEREQUEST), an IME's profile enabled through TSF,
// the window thread's caret (GetGUIThreadInfo) and the windows a process
// shows (an IME's candidate window).
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class ImeInput {
  [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort vk, scan; public uint flags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Explicit)] struct INPUT {
    [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki;
    [FieldOffset(8)] public long pad0; [FieldOffset(16)] public long pad1; [FieldOffset(24)] public long pad2; [FieldOffset(32)] public long pad3; }
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] struct GUITHREADINFO { public int cbSize; public uint flags; public IntPtr active, focus, capture, menuOwner, moveSize, caret; public RECT rcCaret; }
  [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
  [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
  [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] static extern uint MapVirtualKeyW(uint code, uint type);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr LoadKeyboardLayoutW(string id, uint flags);
  [DllImport("user32.dll")] static extern IntPtr GetKeyboardLayout(uint thread);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern bool GetGUIThreadInfo(uint tid, ref GUITHREADINFO info);
  [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);

  [ComImport, Guid("71C6E74C-0F28-11D8-A82A-00065B84435C"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface ITfInputProcessorProfileMgr {
    [PreserveSig] int ActivateProfile(uint type, ushort langid, ref Guid clsid, ref Guid profile, IntPtr hkl, uint flags);
  }
  [ComImport, Guid("33C53A50-F456-4884-B049-85FD643ECFED")] class TfInputProcessorProfiles {}

  // Makes `hwnd` the foreground window (an Alt press lets this process set it).
  public static bool Foreground(IntPtr hwnd) {
    for (int i = 0; i < 10 && GetForegroundWindow() != hwnd; i++) {
      keybd_event(0x12, 0, 0, IntPtr.Zero);
      keybd_event(0x12, 0, 2, IntPtr.Zero);
      BringWindowToTop(hwnd);
      SetForegroundWindow(hwnd);
      Thread.Sleep(300);
    }
    return GetForegroundWindow() == hwnd;
  }

  static INPUT Key(ushort vk, ushort scan, uint flags) {
    var i = new INPUT { type = 1 };
    i.ki.vk = vk; i.ki.scan = scan; i.ki.flags = flags;
    return i;
  }

  static uint Send(List<INPUT> list) {
    return SendInput((uint) list.Count, list.ToArray(), Marshal.SizeOf(typeof(INPUT)));
  }

  // Text as Unicode input (KEYEVENTF_UNICODE: VK_PACKET), as the emoji panel,
  // dictation and on-screen keyboards type.
  public static uint Unicode(string text) {
    var list = new List<INPUT>();
    foreach (char c in text) { list.Add(Key(0, c, 4)); list.Add(Key(0, c, 4 | 2)); }
    return Send(list);
  }

  static void AddKey(List<INPUT> list, ushort vk, bool up) {
    ushort scan = (ushort) MapVirtualKeyW(vk, 0);
    bool extended = vk == 0x23 || vk == 0x24 || vk == 0x25 || vk == 0x26 || vk == 0x27 || vk == 0x28 || vk == 0x2D || vk == 0x2E;
    list.Add(Key(vk, scan, (up ? 2u : 0u) | (extended ? 1u : 0u)));
  }

  // Virtual keys pressed and released one after the other, as the keyboard
  // (and the thread's keyboard layout) types them.
  public static uint Keys(params ushort[] vks) {
    var list = new List<INPUT>();
    foreach (ushort vk in vks) { AddKey(list, vk, false); AddKey(list, vk, true); }
    return Send(list);
  }

  // `vk` with modifiers held (0x11 Ctrl, 0x10 Shift, 0x12 Alt).
  public static uint Chord(ushort[] modifiers, ushort vk) {
    var list = new List<INPUT>();
    foreach (ushort m in modifiers) AddKey(list, m, false);
    AddKey(list, vk, false); AddKey(list, vk, true);
    for (int i = modifiers.Length - 1; i >= 0; i--) AddKey(list, modifiers[i], true);
    return Send(list);
  }

  // Switches the keyboard layout of `hwnd`'s thread (KLID such as "0000040D").
  public static long SwitchLayout(IntPtr hwnd, string klid) {
    IntPtr hkl = LoadKeyboardLayoutW(klid, 0);
    PostMessageW(hwnd, 0x0050 /* WM_INPUTLANGCHANGEREQUEST */, IntPtr.Zero, hkl);
    return hkl.ToInt64();
  }

  public static long ThreadLayout(IntPtr hwnd) {
    uint pid;
    return GetKeyboardLayout(GetWindowThreadProcessId(hwnd, out pid)).ToInt64();
  }

  // Enables (for the user) and activates (here) Microsoft IME for Japanese, so
  // that a window switched to Japanese gets it. HRESULT.
  public static int EnableJapaneseIme() {
    try {
      var mgr = (ITfInputProcessorProfileMgr) new TfInputProcessorProfiles();
      Guid clsid = new Guid("03B5835F-F03C-411B-9CE2-AA23E1171E36");
      Guid profile = new Guid("A76C93D9-5523-4E90-AAFA-4DB112F9AC76");
      return mgr.ActivateProfile(1 /* TF_PROFILETYPE_INPUTPROCESSOR */, 0x411, ref clsid, ref profile,
                                 IntPtr.Zero, 1 /* ENABLEPROFILE */ | 4 /* DONTCARECURRENTINPUTLANGUAGE */);
    } catch (Exception e) {
      return Marshal.GetHRForException(e);
    }
  }

  // The caret of `hwnd`'s thread: {x, y, right, bottom} in its client area and
  // the screen point of its top-left; null when it has none.
  public static int[] Caret(IntPtr hwnd) {
    uint pid;
    uint tid = GetWindowThreadProcessId(hwnd, out pid);
    var g = new GUITHREADINFO { cbSize = Marshal.SizeOf(typeof(GUITHREADINFO)) };
    if (!GetGUIThreadInfo(tid, ref g) || g.caret == IntPtr.Zero) return null;
    var p = new POINT { X = g.rcCaret.Left, Y = g.rcCaret.Top };
    ClientToScreen(g.caret, ref p);
    return new int[] { g.rcCaret.Left, g.rcCaret.Top, g.rcCaret.Right, g.rcCaret.Bottom, p.X, p.Y };
  }

  // The visible top-level windows of process `pid`: "class@x,y,w,h" each.
  public static string[] ProcessWindows(uint pid) {
    var found = new List<string>();
    EnumWindows((h, l) => {
      uint owner;
      GetWindowThreadProcessId(h, out owner);
      if (owner == pid && IsWindowVisible(h)) {
        var cls = new StringBuilder(256);
        GetClassNameW(h, cls, 256);
        RECT r;
        GetWindowRect(h, out r);
        found.Add(cls + "@" + r.Left + "," + r.Top + "," + (r.Right - r.Left) + "," + (r.Bottom - r.Top));
      }
      return true;
    }, IntPtr.Zero);
    return found.ToArray();
  }
}
