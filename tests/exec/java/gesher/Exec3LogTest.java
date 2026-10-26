// Actual Java and NDK logs through the shared logcat file.
package gesher;
import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.InputStreamReader;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;

public final class Exec3LogTest {
    private static String command(String... args) throws Exception {
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
            Thread reader = new Thread(output, "logcat-output");
            reader.setDaemon(true);
            reader.start();
            if (!process.waitFor(15, TimeUnit.SECONDS)) throw new AssertionError(String.join(" ", args) + " timed out");
            String text = output.get(5, TimeUnit.SECONDS);
            if (process.exitValue() != 0) throw new AssertionError(String.join(" ", args) + " exit " + process.exitValue() + ": " + text);
            return text;
        } finally {
            process.destroyForcibly();
        }
    }
    private static void equal(String wanted, String actual) {
        if (!wanted.equals(actual)) throw new AssertionError("expected <" + wanted + ">, got <" + actual + ">");
    }
    private static void contains(String output, String wanted) {
        if (!output.contains(wanted)) throw new AssertionError("missing <" + wanted + "> in <" + output + ">");
    }
    private static String line(BufferedReader input) throws Exception {
        FutureTask<String> task = new FutureTask<>(input::readLine);
        Thread reader = new Thread(task, "logcat-follow-output");
        reader.setDaemon(true);
        reader.start();
        return task.get(10, TimeUnit.SECONDS);
    }
    public static void main(String[] args) throws Exception {
        if (args.length > 1 && args[1].equals("--load-runtime")) System.loadLibrary("android_runtime");
        if (System.getenv("GESHER_LOG_FILE") == null) throw new AssertionError("app did not receive its logcat file");
        Class<?> log = Class.forName("android.util.Log");
        Method println = log.getMethod("println", int.class, String.class, String.class);
        Method loggable = log.getMethod("isLoggable", String.class, int.class);
        Method set = Class.forName("android.os.SystemProperties").getMethod("set", String.class, String.class);
        String child = new File(args[0], "libexec2child.so").getPath();
        String tag = "Exec3Log" + Long.toString(System.nanoTime(), 36).replace('-', 'x');
        command("logcat", "-c");
        println.invoke(null, 4, tag, "java-one");
        println.invoke(null, 5, tag, "java-two\njava-three");
        command(child, "log-write", tag, "6", "native-one");
        command(child, "log-write", tag + "Other", "4", "other-one");
        equal("java-one\njava-two\njava-three\nnative-one\n", command("logcat", "-d", "-v", "raw", "-s", tag));
        equal("java-two\njava-three\nnative-one\n", command("logcat", "-d", "-v", "raw", tag + ":W", "*:S"));
        contains(command("logcat", "-d", "-s", tag, "-v", "brief"), "I/" + tag + "(");
        contains(command("logcat", "-d", "-s", tag, "-v", "tag"), "I/" + tag + ": java-one");
        contains(command("logcat", "-d", "-s", tag, "-v", "process"), ") java-one");
        contains(command("logcat", "-d", "-s", tag, "-v", "thread"), ") java-one");
        contains(command("logcat", "-d", "-s", tag, "-v", "time"), " I/" + tag + "(");
        contains(command("logcat", "-d", "-s", tag, "-v", "threadtime"), " I " + tag + ": java-one");
        contains(command("logcat", "-d", "-s", tag, "-v", "epoch"), " I " + tag + ": java-one");
        contains(command("logcat", "-d", "-s", tag, "-v", "long"), " I/" + tag + " ]\njava-one");
        set.invoke(null, "log.tag." + tag, "E");
        if ((Boolean)loggable.invoke(null, tag, 4) || !(Boolean)loggable.invoke(null, tag, 6)) throw new AssertionError("Java ignores the live tag property");
        println.invoke(null, 4, tag, "java-hidden");
        command(child, "log-write", tag, "3", "native-hidden");
        command(child, "log-write", tag, "6", "native-visible");
        String filtered = command("logcat", "-d", "-v", "raw", "-s", tag);
        if (filtered.contains("hidden")) throw new AssertionError("suppressed Java/native messages were recorded");
        contains(filtered, "native-visible\n");
        contains(command(child, "log-abi", tag + "Abi"), "log-abi:PASS");
        System.out.println("Exec3LogTest PASS actual Java/NDK records, tags, priority filters, nine formats and live loggability properties");
        command("logcat", "-c");
        equal("", command("logcat", "-d", "-v", "raw", "-s", tag));
        set.invoke(null, "log.tag." + tag, "V");
        Process follow = new ProcessBuilder("logcat", "-v", "raw", "-s", tag).redirectErrorStream(true).start();
        try {
            follow.getOutputStream().close();
            BufferedReader input = new BufferedReader(new InputStreamReader(follow.getInputStream(), StandardCharsets.UTF_8));
            println.invoke(null, 4, tag, "after-clear");
            equal("after-clear", line(input));
            command("logcat", "-c");
            println.invoke(null, 4, tag, "after-second-clear");
            equal("after-second-clear", line(input));
        } finally {
            follow.destroyForcibly();
            if (!follow.waitFor(5, TimeUnit.SECONDS)) throw new AssertionError("logcat follower did not exit");
        }
        System.out.println("Exec3LogTest PASS clear removes old records and a live follower observes new records after clear");
    }
}
