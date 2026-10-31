// Source only locally. Compiled and executed exclusively by Windows CI.
using System;
using System.Diagnostics;
using System.IO;
using System.Threading;

public class Graphics84 {
  static int Main(string[] args) {
    if (Environment.GetEnvironmentVariable("GITHUB_ACTIONS") != "true") return 90;
    if (args.Length == 2 && args[0] == "hold") { Thread.Sleep(60000); return 0; }
    string mode = Environment.GetEnvironmentVariable("GESHER_GRAPHICS84_MODE");
    string receipt = Environment.GetEnvironmentVariable("GESHER_GRAPHICS84_RECEIPT");
    string[] expected = File.ReadAllLines(Environment.GetEnvironmentVariable("GESHER_GRAPHICS84_ARGS"));
    if (args.Length != expected.Length) return 91;
    for (int i = 0; i < args.Length; ++i) if (args[i] != expected[i]) return 92;
    if (Environment.CurrentDirectory != Environment.GetEnvironmentVariable("GESHER_GRAPHICS84_CWD")) return 93;
    if (Environment.GetEnvironmentVariable("GESHER_TRACE_FAULTS") != "1") return 94;
    using (var self = Process.GetCurrentProcess()) {
      File.WriteAllLines(receipt, new[] { self.Id.ToString(), self.StartTime.ToUniversalTime().Ticks.ToString(),
        self.MainModule.FileName, Environment.CurrentDirectory, "argv-and-env-exact" });
    }
    Console.Error.WriteLine("graphics84 actual stderr " + mode);
    if (mode == "timeout") { Thread.Sleep(300000); return 0; }
    if (mode != "missing-marker") Console.WriteLine("GraphicsFixture all passed");
    Console.WriteLine("PASS  graphics84 actual fixture " + mode);
    if (mode == "retained") {
      int result = Process84.RetainOutput(receipt + ".child");
      if (result != 0) return result;
      int pid = Int32.Parse(File.ReadAllText(receipt + ".child"));
      using (var child = Process.GetProcessById(pid)) {
        File.WriteAllLines(receipt + ".child", new[] { pid.ToString(), child.StartTime.ToUniversalTime().Ticks.ToString(), child.MainModule.FileName });
      }
    }
    return mode == "nonzero" ? 23 : 0;
  }
}
