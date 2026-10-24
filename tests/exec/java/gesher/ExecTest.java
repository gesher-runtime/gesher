// Processes from Java on Windows (docs/exec.md): ProcessBuilder and
// Runtime.exec through libcore's UNIXProcess natives (compat/libcore/process.cc)
// running an Android ELF executable (libexectest.so, tests/exec/exec_test.c)
// in gesher-exec.exe, the minimal sh, and Windows programs.
//
//   dalvikvm ... -cp exec-test.dex gesher.ExecTest <dir with libexectest.so>
//
// Prints one line per check and "ExecTest all passed" when all pass.
package gesher;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.Map;

public class ExecTest {
  static int failures = 0;

  static void check(String what, boolean ok, String detail) {
    System.out.println((ok ? "ok    " : "FAIL  ") + what + (ok || detail == null ? "" : "  [" + detail + "]"));
    if (!ok) failures++;
  }

  static byte[] readAll(InputStream in) throws IOException {
    ByteArrayOutputStream out = new ByteArrayOutputStream();
    byte[] buf = new byte[8192];
    int n;
    while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
    return out.toByteArray();
  }

  static String str(byte[] b) { return new String(b, StandardCharsets.UTF_8); }

  // Reads a stream on its own thread (both pipes at once, as apps must).
  static final class Drain extends Thread {
    final InputStream in;
    volatile byte[] data = new byte[0];
    Drain(InputStream in) { this.in = in; start(); }
    @Override public void run() {
      try { data = readAll(in); } catch (IOException e) { data = ("IOException " + e).getBytes(); }
    }
    String text() throws InterruptedException { join(); return str(data); }
  }

  static final class Result {
    int status; String out; String err;
    @Override public String toString() { return "status " + status + ", stdout <" + out + ">, stderr <" + err + ">"; }
  }

  static Result run(ProcessBuilder pb) throws Exception {
    Process p = pb.start();
    p.getOutputStream().close();
    Drain out = new Drain(p.getInputStream());
    Drain err = new Drain(p.getErrorStream());
    Result r = new Result();
    r.status = p.waitFor();
    r.out = out.text();
    r.err = err.text();
    return r;
  }

  public static void main(String[] args) throws Exception {
    String dir = args[0];
    File exe = new File(dir, "libexectest.so");
    String prog = exe.getPath();
    check("File.canExecute() of the ELF executable", exe.canExecute(), prog);

    // argv (spaces, quotes, backslashes, empty, non-ASCII), the environment,
    // the working directory, LD_LIBRARY_PATH for its DT_NEEDED library.
    ProcessBuilder pb = new ProcessBuilder(prog, "echo", "a b", "", "q\"uote", "back\\slash\\", "שלום");
    pb.environment().put("EXEC_TEST_VAR", "value with spaces");
    pb.environment().put("LD_LIBRARY_PATH", dir);
    File work = new File(dir, "exec-test-cwd");
    work.mkdirs();
    pb.directory(work);
    Result r = run(pb);
    check("ELF executable runs: exit status 0", r.status == 0, r.toString());
    check("argc and argv", r.out.contains("argc=7\n") && r.out.contains("argv[0]=<" + prog + ">\n")
        && r.out.contains("argv[2]=<a b>\n") && r.out.contains("argv[3]=<>\n")
        && r.out.contains("argv[4]=<q\"uote>\n") && r.out.contains("argv[5]=<back\\slash\\>\n")
        && r.out.contains("argv[6]=<שלום>\n"), r.out);
    check("environment: getenv and environ", r.out.contains("getenv=<value with spaces>\n")
        && r.out.contains("environ has EXEC_TEST_VAR=1\n"), r.out);
    String cwdName = work.getName();
    check("working directory", r.out.matches("(?s).*cwd=<[^>]*" + cwdName + ">\n.*"), r.out);
    check("__progname, getauxval (AT_PAGESZ, AT_PHDR, AT_RANDOM)", r.out.contains("progname=<libexectest.so>\n")
        && r.out.contains("pagesz=4096 phdr=set random=set\n"), r.out);
    check("init function got (argc, argv, envp)", r.out.contains("ctor argc=7\n"), r.out);
    check("DT_NEEDED library from LD_LIBRARY_PATH, its constructor ran",
        r.out.contains("helper=<hello from libexechelper (constructor ran)>\n"), r.out);
    check("a pthread and thread-locals in the executable", r.out.contains("thread=100 tls=7\n"), r.out);
    check("nothing on stderr", r.err.isEmpty(), r.err);

    // Puzzles' pattern: waitFor() first, then read stdout (the reaper drains
    // the pipe into the stream when the process exits).
    {
      Process p = new ProcessBuilder(prog, "big", "30000").start();
      p.getOutputStream().close();
      int status = p.waitFor();
      byte[] out = readAll(p.getInputStream());
      byte[] err = readAll(p.getErrorStream());
      boolean pattern = out.length == 30000;
      for (int i = 0; pattern && i < out.length; i++) pattern = out[i] == (byte) ('a' + i % 26);
      check("waitFor() then read: 30000 bytes of output kept", status == 0 && pattern && err.length == 0,
          "status " + status + ", " + out.length + " bytes");
    }
    // Streaming far more than a pipe holds.
    {
      Process p = new ProcessBuilder(prog, "big", "3000000").start();
      Drain err = new Drain(p.getErrorStream());
      byte[] out = readAll(p.getInputStream());
      int status = p.waitFor();
      boolean pattern = out.length == 3000000;
      for (int i = 0; pattern && i < out.length; i++) pattern = out[i] == (byte) ('a' + i % 26);
      check("3,000,000 bytes streamed through stdout", status == 0 && pattern, "status " + status + ", "
          + out.length + " bytes, stderr " + err.text());
    }
    // stdin: 200 KB written while stdout is read.
    {
      Process p = new ProcessBuilder(prog, "cat").start();
      Drain out = new Drain(p.getInputStream());
      Drain err = new Drain(p.getErrorStream());
      byte[] data = new byte[200000];
      for (int i = 0; i < data.length; i++) data[i] = (byte) (i * 7 + i / 300);
      OutputStream in = p.getOutputStream();
      in.write(data);
      in.close();
      int status = p.waitFor();
      out.text();
      String e = err.text();
      check("stdin to stdout: 200000 bytes round trip", status == 0 && Arrays.equals(out.data, data)
          && e.equals("cat: 200000 bytes\n"), "status " + status + ", " + out.data.length + " bytes back, stderr " + e);
    }

    // Exit statuses: exit(n) with atexit handlers and destructors; abort();
    // a fault.
    r = run(new ProcessBuilder(prog, "exit", "3"));
    check("exit(3): status 3, atexit handler and destructor ran", r.status == 3
        && r.out.contains("exiting with 3\n") && r.out.contains("atexit ran\n") && r.out.contains("destructor ran\n"), r.toString());
    r = run(new ProcessBuilder(prog, "exit", "300"));
    check("exit(300): status 44 (the low 8 bits)", r.status == 44, r.toString());
    r = run(new ProcessBuilder(prog, "abort"));
    check("abort(): status 134 (0x80 + SIGABRT)", r.status == 134 && r.out.contains("aborting\n"), r.toString());
    r = run(new ProcessBuilder(prog, "segv"));
    check("a fault: status 139 (0x80 + SIGSEGV)", r.status == 139 && r.out.contains("faulting\n"), r.toString());

    // destroy(): 0x80 + SIGTERM.
    {
      Process p = new ProcessBuilder(prog, "sleep").start();
      InputStream in = p.getInputStream();
      byte[] first = new byte[9];
      int got = 0;
      while (got < first.length) {
        int n = in.read(first, got, first.length - got);
        if (n <= 0) break;
        got += n;
      }
      boolean alive;
      try { p.exitValue(); alive = false; } catch (IllegalThreadStateException e) { alive = true; }
      long t0 = System.nanoTime();
      p.destroy();
      int status = p.waitFor();
      long ms = (System.nanoTime() - t0) / 1000000;
      check("destroy() of a sleeping program: status 143 (0x80 + SIGTERM)", alive && str(Arrays.copyOf(first, got)).equals("sleeping\n")
          && status == 143 && ms < 10000, "alive " + alive + ", read <" + str(Arrays.copyOf(first, got)) + ">, status " + status + " after " + ms + " ms");
    }

    // redirectErrorStream and redirecting to a file.
    {
      ProcessBuilder b = new ProcessBuilder(prog, "stderr").redirectErrorStream(true);
      r = run(b);
      check("redirectErrorStream(true): stderr in stdout", r.status == 0 && r.out.equals("to stdout\nto stderr\n") && r.err.isEmpty(), r.toString());
      File f = new File(work, "out.txt");
      f.delete();
      Process p = new ProcessBuilder(prog, "stderr").redirectOutput(f).start();
      String err = str(readAll(p.getErrorStream()));
      int status = p.waitFor();
      String fileText = new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
      check("redirectOutput(file)", status == 0 && fileText.equals("to stdout\n") && err.equals("to stderr\n"), "status " + status + ", file <" + fileText + ">, stderr <" + err + ">");
    }

    // A program that does not exist; a directory.
    try {
      new ProcessBuilder(new File(dir, "no-such-program").getPath()).start();
      check("a missing program: IOException", false, "started");
    } catch (IOException e) {
      check("a missing program: IOException error=2", String.valueOf(e.getMessage()).contains("error=2, No such file or directory"), e.getMessage());
    }
    try {
      new ProcessBuilder(dir).start();
      check("a directory: IOException", false, "started");
    } catch (IOException e) {
      check("a directory: IOException error=13", String.valueOf(e.getMessage()).contains("error=13"), e.getMessage());
    }

    // Runtime.exec and the minimal sh.
    {
      Process p = Runtime.getRuntime().exec(new String[] {prog, "exit", "5"});
      p.getOutputStream().close();
      int status = p.waitFor();
      String out = str(readAll(p.getInputStream()));
      check("Runtime.exec(String[])", status == 5 && out.contains("exiting with 5\n"), "status " + status + ", " + out);
    }
    r = run(new ProcessBuilder("sh", "-c", "echo hello $EXEC_SH_VAR; false || echo or-ran; true && echo and-ran; exit 7")
        .redirectErrorStream(false));
    check("sh -c: echo, $VAR, ||, &&, exit 7", r.status == 7 && r.out.equals("hello\nor-ran\nand-ran\n"), r.toString());
    {
      ProcessBuilder b = new ProcessBuilder("/system/bin/sh", "-c", "\"" + prog + "\" echo from-sh");
      b.environment().put("LD_LIBRARY_PATH", dir);
      r = run(b);
      check("/system/bin/sh -c runs the ELF executable", r.status == 0 && r.out.contains("argv[2]=<from-sh>\n"), r.toString());
    }
    r = run(new ProcessBuilder("sh", "-c", "no-such-command-gesher"));
    check("sh: a missing command is status 127", r.status == 127 && r.err.contains("not found"), r.toString());
    r = run(new ProcessBuilder("sh", "-c", "echo a | cat"));
    check("sh: a pipe is refused with status 2", r.status == 2 && r.err.contains("not supported"), r.toString());
    {
      Process p = new ProcessBuilder("sh").start();
      Drain out = new Drain(p.getInputStream());
      Drain err = new Drain(p.getErrorStream());
      OutputStream in = p.getOutputStream();
      in.write(("echo line1\n\"" + prog + "\" exit 4\necho status $?\nexit 9\necho never\n").getBytes(StandardCharsets.UTF_8));
      in.close();
      int status = p.waitFor();
      String o = out.text();
      check("sh reading commands from stdin", status == 9 && o.startsWith("line1\n") && o.contains("status 4\n")
          && !o.contains("never"), "status " + status + ", stdout <" + o + ">, stderr <" + err.text() + ">");
    }
    {
      File script = new File(work, "script.sh");
      try (FileOutputStream f = new FileOutputStream(script)) {
        f.write("#!/system/bin/sh\necho script $1\nexit 6\n".getBytes(StandardCharsets.UTF_8));
      }
      r = run(new ProcessBuilder(script.getPath(), "arg1"));
      check("a #!/system/bin/sh script", r.status == 6 && r.out.equals("script arg1\n"), r.toString());
    }

    // Windows programs: cmd.exe through PATH, a .cmd file.
    r = run(new ProcessBuilder("cmd", "/c", "echo win32"));
    check("cmd /c echo (a Windows .exe from PATH)", r.status == 0 && r.out.trim().equals("win32"), r.toString());
    {
      File batch = new File(work, "t.cmd");
      try (FileOutputStream f = new FileOutputStream(batch)) {
        f.write("@echo off\r\necho batch %~1\r\nexit /b 4\r\n".getBytes(StandardCharsets.UTF_8));
      }
      r = run(new ProcessBuilder(batch.getPath(), "x y"));
      check("a .cmd file through cmd.exe", r.status == 4 && r.out.trim().equals("batch x y"), r.toString());
    }

    // The environment: ProcessBuilder's copy, changed.
    {
      ProcessBuilder b = new ProcessBuilder(prog, "echo");
      Map<String, String> env = b.environment();
      env.remove("EXEC_TEST_VAR");
      env.put("LD_LIBRARY_PATH", dir);
      r = run(b);
      check("environment without the variable", r.status == 0 && r.out.contains("getenv=<(null)>\n"), r.toString());
    }

    System.out.println(failures == 0 ? "ExecTest all passed" : ("ExecTest " + failures + " check(s) failed"));
    System.exit(failures == 0 ? 0 : 1);
  }
}
