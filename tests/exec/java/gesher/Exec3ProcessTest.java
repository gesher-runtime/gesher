// Real process signals and /system/bin tools through ART's ProcessBuilder.
package gesher;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;

public final class Exec3ProcessTest {
    private static String line(InputStream input) throws Exception {
        FutureTask<String> read = new FutureTask<>(() -> new BufferedReader(
                new InputStreamReader(input, StandardCharsets.UTF_8)).readLine());
        Thread thread = new Thread(read, "exec3-child-ready");
        thread.setDaemon(true);
        thread.start();
        return read.get(20, TimeUnit.SECONDS);
    }

    private static void status(Process process, int wanted) throws Exception {
        if (!process.waitFor(20, TimeUnit.SECONDS)) throw new AssertionError("child did not exit");
        if (process.exitValue() != wanted) {
            throw new AssertionError("expected status " + wanted + ", got " + process.exitValue());
        }
    }

    private static byte[] bytes(InputStream input) throws Exception {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        int count;
        while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
        return output.toByteArray();
    }

    private static Process child(String program) throws Exception {
        Process process = new ProcessBuilder(program, "sigloop-pid").start();
        process.getOutputStream().close();
        return process;
    }

    public static void main(String[] args) throws Exception {
        String program = new File(args[0], "libexec2child.so").getPath();
        Process process = child(program);
        try {
            String ready = line(process.getInputStream());
            if (ready == null || !ready.startsWith("child-ready:")) throw new AssertionError(ready);
            int pid = Integer.parseInt(ready.substring("child-ready:".length()));
            Process ps = new ProcessBuilder("/system/bin/ps").start();
            ps.getOutputStream().close();
            String list = new String(bytes(ps.getInputStream()), StandardCharsets.UTF_8);
            status(ps, 0);
            boolean found = false;
            for (String row : list.split("\n")) {
                String[] columns = row.trim().split("\\s+");
                if (columns.length >= 3 && columns[0].equals(Integer.toString(pid))) found = true;
            }
            if (!found) throw new AssertionError("ps omitted child " + pid + ": " + list);
            process.destroy();
            status(process, 43); // sigloop's SIGTERM handler calls _exit(43).
            System.out.println("Exec3ProcessTest PASS destroy runs the native SIGTERM handler and ps lists its pid");
        } finally {
            process.destroyForcibly();
        }

        process = child(program);
        try {
            String ready = line(process.getInputStream());
            if (ready == null || !ready.startsWith("child-ready:")) throw new AssertionError(ready);
            process.destroyForcibly();
            status(process, 137); // The SIGTERM handler must not turn SIGKILL into 43.
            System.out.println("Exec3ProcessTest PASS destroyForcibly kills with status 137");
        } finally {
            process.destroyForcibly();
        }

        process = child(program);
        try {
            String ready = line(process.getInputStream());
            if (ready == null || !ready.startsWith("child-ready:")) throw new AssertionError(ready);
            String pid = ready.substring("child-ready:".length());
            Process kill = new ProcessBuilder("/system/bin/kill", "-TERM", pid).start();
            kill.getOutputStream().close();
            status(kill, 0);
            status(process, 43);
            System.out.println("Exec3ProcessTest PASS kill sends SIGTERM through the native signal mailbox");
        } finally {
            process.destroyForcibly();
        }

        Process cat = new ProcessBuilder("/system/bin/cat").start();
        try {
            byte[] wanted = new byte[10000];
            for (int i = 0; i < wanted.length; ++i) wanted[i] = (byte) (i * 13);
            cat.getOutputStream().write(wanted);
            cat.getOutputStream().close();
            byte[] actual = bytes(cat.getInputStream());
            status(cat, 0);
            if (!Arrays.equals(actual, wanted)) throw new AssertionError("cat changed binary stdin");
            System.out.println("Exec3ProcessTest PASS cat preserves binary stdin and stdout");
        } finally {
            cat.destroyForcibly();
        }

        long start = System.nanoTime();
        Process sleep = new ProcessBuilder("/system/bin/sleep", "0.25").start();
        try {
            sleep.getOutputStream().close();
            status(sleep, 0);
            long elapsed = TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - start);
            if (elapsed < 200) throw new AssertionError("sleep returned early: " + elapsed + " ms");
            System.out.println("Exec3ProcessTest PASS sleep delays for the requested duration");
        } finally {
            sleep.destroyForcibly();
        }
    }
}
