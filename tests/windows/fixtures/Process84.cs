// CI-only processes with real inherited output handles.
using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public class Process84 {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  public struct Startup {
    public int cb;
    public string reserved, desktop, title;
    public int x, y, width, height, charsX, charsY, fill, flags;
    public short show, reservedSize;
    public IntPtr reservedData, input, output, error;
  }
  [StructLayout(LayoutKind.Sequential)]
  public struct ProcessInfo {
    public IntPtr process, thread;
    public uint pid, tid;
  }
  [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
  [DllImport("kernel32.dll")] static extern IntPtr GetStdHandle(int which);
  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool DuplicateHandle(IntPtr sourceProcess, IntPtr source, IntPtr targetProcess,
      out IntPtr result, uint access, bool inherit, uint options);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern bool CreateProcess(string application, StringBuilder command, IntPtr processAttributes,
      IntPtr threadAttributes, bool inherit, uint flags, IntPtr environment, string cwd,
      ref Startup startup, out ProcessInfo process);

  static int Main(string[] args) {
    if (args.Length != 2) return 2;
    Console.WriteLine("process84 stdout " + args[0]);
    Console.Error.WriteLine("process84 stderr " + args[0]);
    if (args[0] == "normal") return 0;
    if (args[0] == "nonzero") return 23;
    if (args[0] == "environment") {
      Console.WriteLine("process84 cwd " + Environment.CurrentDirectory);
      Console.WriteLine("process84 environment " + Environment.GetEnvironmentVariable("GESHER_PROCESS84_ENV"));
      return 0;
    }
    if (args[0] == "hold") { Thread.Sleep(60000); return 0; }
    if (args[0] == "timeout") {
      File.WriteAllText(args[1], System.Diagnostics.Process.GetCurrentProcess().Id.ToString());
      Thread.Sleep(300000);
      return 0;
    }
    if (args[0] != "retained") return 2;
    IntPtr output, error;
    IntPtr current = GetCurrentProcess();
    if (!DuplicateHandle(current, GetStdHandle(-11), current, out output, 0, true, 2)) return 3;
    if (!DuplicateHandle(current, GetStdHandle(-12), current, out error, 0, true, 2)) {
      CloseHandle(output);
      return 3;
    }
    try {
      var startup = new Startup();
      startup.cb = Marshal.SizeOf(typeof(Startup));
      startup.flags = 0x100;
      startup.output = output;
      startup.error = error;
      ProcessInfo child;
      string self = Assembly.GetExecutingAssembly().Location;
      if (!CreateProcess(self, new StringBuilder("\"" + self + "\" hold unused"), IntPtr.Zero, IntPtr.Zero,
          true, 0x08000000, IntPtr.Zero, null, ref startup, out child)) return 4;
      File.WriteAllText(args[1], child.pid.ToString());
      CloseHandle(child.thread);
      CloseHandle(child.process);
    } finally { CloseHandle(output); CloseHandle(error); }
    return 0;
  }
}
