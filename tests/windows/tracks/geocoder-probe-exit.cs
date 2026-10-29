
using System;
using System.Runtime.InteropServices;
public static class GesherGeocoderProbeExit {
  [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
  [DllImport("kernel32.dll", SetLastError = true)]
  [return: MarshalAs(UnmanagedType.Bool)]
  static extern bool TerminateProcess(IntPtr process, uint code);
  public static void Exit(uint code) {
    if (!TerminateProcess(GetCurrentProcess(), code))
      throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
  }
}
