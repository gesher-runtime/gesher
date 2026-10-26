// Observe pm/am through actual Binder services and a separate installed APK.
package gesher;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public final class Exec3CommandsTest {
    private static String command(int wanted, String... args) throws Exception {
        Process process = new ProcessBuilder(args).redirectErrorStream(true).start();
        try {
            process.getOutputStream().close();
            FutureTask<String> output = new FutureTask<>(() -> {
                ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                byte[] buffer = new byte[8192];
                int count;
                while ((count = process.getInputStream().read(buffer)) >= 0) bytes.write(buffer, 0, count);
                return bytes.toString("UTF-8").replace("\r\n", "\n");
            });
            Thread reader = new Thread(output, "device-command-output");
            reader.setDaemon(true);
            reader.start();
            if (!process.waitFor(30, TimeUnit.SECONDS)) throw new AssertionError(String.join(" ", args) + " timed out");
            String text = output.get(5, TimeUnit.SECONDS);
            if (process.exitValue() != wanted) throw new AssertionError(String.join(" ", args) + " exit " + process.exitValue() + ": " + text);
            return text;
        } finally {
            process.destroyForcibly();
        }
    }
    private static String log(File file) throws Exception {
        return file.isFile() ? new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8) : "";
    }
    private static String waitForLog(File file, String marker) throws Exception {
        long until = System.nanoTime() + TimeUnit.SECONDS.toNanos(20);
        do {
            String text = log(file);
            if (text.contains(marker)) return text;
            Thread.sleep(50);
        } while (System.nanoTime() < until);
        throw new AssertionError("missing " + marker + " in " + log(file));
    }
    public static void main(String[] args) throws Exception {
        if (System.getenv("GESHER_BINDER") == null) throw new AssertionError("commands test needs the actual device");
        String logs = System.getenv("GESHER_LOGS");
        if (logs == null) throw new AssertionError("target app log directory is unavailable");
        String target = "com.gesher.exec3target";
        File targetLog = new File(logs, target + ".log");
        String packages = command(0, "/system/bin/pm", "list", "packages", target);
        if (!packages.contains("package:" + target + "\n")) throw new AssertionError("pm did not query the installed target: " + packages);
        String pathOutput = command(0, "pm", "path", target);
        Matcher path = Pattern.compile("(?m)^package:(.+)$").matcher(pathOutput);
        if (!path.find() || !new File(path.group(1)).isFile()) throw new AssertionError("pm path did not return actual installed code: " + pathOutput);
        command(1, "pm", "path", "com.gesher.exec3.missing");
        System.out.println("Exec3CommandsTest PASS pm list packages and path query the actual device package manager");
        command(0, "/system/bin/am", "start", "-n", target + "/gesher.TargetActivity");
        String started = waitForLog(targetLog, "Exec3Target READY pid=");
        Matcher pid = Pattern.compile("Exec3Target READY pid=(\\d+)").matcher(started);
        if (!pid.find()) throw new AssertionError("target did not resume");
        int processId = Integer.parseInt(pid.group(1));
        String processes = command(0, "ps");
        if (!Pattern.compile("(?m)^" + processId + "\\s+").matcher(processes).find()) throw new AssertionError("started target pid is absent from ps");
        command(0, "am", "broadcast", "-n", target + "/gesher.TargetReceiver", "-a", "com.gesher.exec3.COMMAND_TEST", "--es", "payload", "native-tool");
        waitForLog(targetLog, "Exec3Target BROADCAST action=com.gesher.exec3.COMMAND_TEST payload=native-tool pid=" + processId);
        System.out.println("Exec3CommandsTest PASS am start resumes a separate app and broadcast reaches its real receiver");
        command(0, "am", "force-stop", target);
        long until = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
        while (Pattern.compile("(?m)^" + processId + "\\s+").matcher(command(0, "ps")).find()) {
            if (System.nanoTime() >= until) throw new AssertionError("force-stop left target pid " + processId + " alive");
            Thread.sleep(50);
        }
        System.out.println("Exec3CommandsTest PASS am force-stop terminates the target app process");
    }
}
