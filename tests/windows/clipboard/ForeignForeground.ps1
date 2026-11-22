param([Parameter(Mandatory)][string]$Receipt, [Parameter(Mandatory)][string]$Stop)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
public static class ClipboardForegroundWindow {
  delegate IntPtr WindowProc(IntPtr window, uint message, IntPtr w, IntPtr l);
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct WindowClass {
    public uint Style; public WindowProc Procedure; public int ClassExtra, WindowExtra;
    public IntPtr Instance, Icon, Cursor, Background; public string Menu, Name;
  }
  [StructLayout(LayoutKind.Sequential)] struct Message {
    public IntPtr Window; public uint Id; public IntPtr W, L; public uint Time; public int X, Y; public uint Private;
  }
  [StructLayout(LayoutKind.Sequential)] struct FileTime { public uint Low, High; }
  [DllImport("kernel32.dll")] static extern uint GetCurrentProcessId();
  [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern IntPtr GetModuleHandleW(string name);
  [DllImport("kernel32.dll")] static extern bool GetProcessTimes(IntPtr process, out FileTime created, out FileTime exited, out FileTime kernel, out FileTime user);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern ushort RegisterClassW(ref WindowClass value);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern bool UnregisterClassW(string name, IntPtr instance);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr CreateWindowExW(uint ex, string cls, string title, uint style, int x, int y, int width, int height, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr parameter);
  [DllImport("user32.dll")] static extern IntPtr DefWindowProcW(IntPtr window, uint message, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] static extern bool DestroyWindow(IntPtr window);
  [DllImport("user32.dll")] static extern bool IsWindow(IntPtr window);
  [DllImport("user32.dll")] static extern bool PeekMessageW(out Message message, IntPtr window, uint min, uint max, uint remove);
  [DllImport("user32.dll")] static extern bool TranslateMessage(ref Message message);
  [DllImport("user32.dll")] static extern IntPtr DispatchMessageW(ref Message message);
  static readonly WindowProc Procedure = DefWindowProcW;
  public static void Run(string receipt, string stop) {
    IntPtr instance = GetModuleHandleW(null), window = IntPtr.Zero;
    string name = "GesherClipboardForeground" + GetCurrentProcessId();
    WindowClass cls = new WindowClass(); cls.Name = name; cls.Instance = instance; cls.Procedure = Procedure;
    if (RegisterClassW(ref cls) == 0) throw new InvalidOperationException("RegisterClassW failed");
    try {
      // Initially hidden: creation must not disturb the application's earlier input.
      window = CreateWindowExW(0, name, "Clipboard background access control", 0x00CF0000,
          30, 30, 300, 160, IntPtr.Zero, IntPtr.Zero, instance, IntPtr.Zero);
      if (window == IntPtr.Zero) throw new InvalidOperationException("CreateWindowExW failed");
      FileTime created, exited, kernel, user;
      if (!GetProcessTimes(GetCurrentProcess(), out created, out exited, out kernel, out user)) throw new InvalidOperationException("GetProcessTimes failed");
      ulong generation = ((ulong)created.High << 32) | created.Low;
      string json = "{\"pid\":" + GetCurrentProcessId() + ",\"created\":" + generation + ",\"hwnd\":" + window.ToInt64() + "}";
      File.WriteAllText(receipt + ".new", json); File.Move(receipt + ".new", receipt);
      DateTime deadline = DateTime.UtcNow.AddMinutes(3);
      while (!File.Exists(stop) && IsWindow(window) && DateTime.UtcNow < deadline) {
        Message message;
        while (PeekMessageW(out message, IntPtr.Zero, 0, 0, 1)) { TranslateMessage(ref message); DispatchMessageW(ref message); }
        Thread.Sleep(20);
      }
    } finally {
      if (window != IntPtr.Zero && IsWindow(window) && !DestroyWindow(window)) throw new InvalidOperationException("DestroyWindow failed");
      if (!UnregisterClassW(name, instance)) throw new InvalidOperationException("UnregisterClassW failed");
    }
  }
}
'@
[ClipboardForegroundWindow]::Run($Receipt, $Stop)
