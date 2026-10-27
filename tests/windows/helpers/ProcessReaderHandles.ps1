# Shared CI-only exact owned-reader/native pipe close primitive.
if (-not ('GtestProcessNativeHandles' -as [type])) {
  # This test infrastructure is compiled only when the CI scripts execute.
  Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class GtestProcessNativeHandles {
  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool GetHandleInformation(IntPtr handle, out uint flags);
  static GtestProcessNativeHandles() {
    // Resolve/JIT the P/Invoke before the first close/probe interval.
    uint flags;
    GetHandleInformation(IntPtr.Zero, out flags);
  }
  public static bool CloseOwnedReader(StreamReader reader, SafeFileHandle owned) {
    var file = reader.BaseStream as FileStream;
    bool same = file != null && Object.ReferenceEquals(file.SafeFileHandle, owned);
    IntPtr value = owned.DangerousGetHandle();
    reader.Dispose();
    uint flags;
    // Pair the exact owned SafeFileHandle with its stream BEFORE disposal.
    // No allocation/open occurs between that disposal and the native probe.
    // A concurrently reused value that is valid is conservatively a failure.
    return same && owned.IsClosed && !GetHandleInformation(value, out flags) && Marshal.GetLastWin32Error() == 6;
  }
}
'@
}
