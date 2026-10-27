// Compile/run only on CI. The real gtest helpers provide the actual argv/cwd
// and output pipe handling; this executable supplies controlled child states.
using System;
using System.IO;
using System.Threading;

public class Gtest84 {
  static int Main(string[] args) {
    if (Environment.GetEnvironmentVariable("GITHUB_ACTIONS") != "true") return 90;
    if (args.Length == 2 && args[0] == "hold") { Thread.Sleep(60000); return 0; }
    string mode = Environment.GetEnvironmentVariable("GESHER_GTEST84_MODE");
    string pidFile = Environment.GetEnvironmentVariable("GESHER_GTEST84_PID");
    string expectedCwd = Environment.GetEnvironmentVariable("GESHER_GTEST84_CWD");
    if (Path.GetFullPath(Environment.CurrentDirectory) != Path.GetFullPath(expectedCwd)) return 91;
    if (args.Length != 2 || args[0] != "--gtest_color=no" || args[1] != "--gtest_filter=-Fixture.Excluded") return 92;
    Console.WriteLine("[ RUN      ] Fixture.RealProcess");
    Console.Error.WriteLine("gtest84 actual stderr " + mode);
    if (mode == "timeout") {
      File.WriteAllText(pidFile, System.Diagnostics.Process.GetCurrentProcess().Id.ToString());
      Thread.Sleep(300000);
      return 0;
    }
    if (mode == "failed") Console.WriteLine("[  FAILED  ] Fixture.RealProcess, failure detail");
    else Console.WriteLine("[       OK ] Fixture.RealProcess");
    if (mode != "missing-summary") Console.WriteLine("[  PASSED  ] " + (mode == "zero-count" ? "0" : "1") + " test.");
    if (mode == "retained") {
      // The shared real CreateProcess helper duplicates/inherits BOTH output
      // handles and writes the independent child's PID before this parent exits.
      return Process84.RetainOutput(pidFile);
    }
    return mode == "nonzero" ? 23 : 0;
  }
}
