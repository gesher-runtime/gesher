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
        boolean operator = args.length == 4 && args[1].equals("--app-operator");
        File phase = operator ? new File(args[2]) : null;
        String tag = operator ? Exec3AppPropertiesTest.tag(args[3]) : "Exec3Log" + Long.toString(System.nanoTime(), 36).replace('-', 'x');
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
        if (operator) Exec3AppPropertiesTest.request(phase, "log-error");
        else set.invoke(null, "log.tag." + tag, "E");
        if ((Boolean)loggable.invoke(null, tag, 4) || !(Boolean)loggable.invoke(null, tag, 6)) throw new AssertionError("Java ignores the live tag property");
        println.invoke(null, 4, tag, "java-hidden");
        command(child, "log-write", tag, "3", "native-hidden");
        command(child, "log-write", tag, "6", "native-visible");
        String filtered = command("logcat", "-d", "-v", "raw", "-s", tag);
        if (filtered.contains("hidden")) throw new AssertionError("suppressed Java/native messages were recorded");
        contains(filtered, "native-visible\n");
        if (!operator) contains(command(child, "log-abi", tag + "Abi"), "log-abi:PASS");
        // The full native mutating ABI corpus runs under the standalone
        // operator above. The app validates the real externally set filter.
        if (operator) {
            Exec3AppPropertiesTest.request(phase, "log-info");
            if (!(Boolean)loggable.invoke(null, tag, 4)) throw new AssertionError("Java missed operator filter reset");
        }
        System.out.println("Exec3LogTest PASS actual Java/NDK records, tags, priority filters, nine formats and live loggability properties");
        // Credentials never reach the persisted logcat file or the stderr log (docs/logging.md).
        // The values are synthetic; 45-exec3.ps1 also searches the log files for them.
        String secretTag = tag + "Secret";
        println.invoke(null, 4, secretTag, "GmsAuthenticator getAuthToken: ya29.a0Exec3SYNTHETICjava\n"
                + "LoginRequest{user=7, auth_token=8754Exec3SYNTHETIC}\n\u05e9\u05dc\u05d5\u05dd Bearer Exec3SYNTHETICbearer");
        String nativeOutput = command(child, "log-print", secretTag, "6", "auth_tok", "en=Exec3SYNTH",
                "ETICsplit ya2", "9.Exec3SYNTHETICnative");
        if (nativeOutput.contains("SYNTHETIC")) throw new AssertionError("native stderr log kept a credential: " + nativeOutput);
        contains(nativeOutput, "auth_token=<redacted> ya29.<redacted>");
        equal("GmsAuthenticator getAuthToken: <redacted>\nLoginRequest{user=7, auth_token=<redacted>}\n"
                + "\u05e9\u05dc\u05d5\u05dd Bearer <redacted>\nauth_token=<redacted> ya29.<redacted>\n",
                command("logcat", "-d", "-v", "raw", "-s", secretTag));
        if (args.length == 1 || operator) {
            // In an app process (ExecActivity): stdout/stderr are the package's .log file.
            System.out.print("Exec3 stdout getAuthToken: ya2");
            System.out.print("9.a0Exec3SYNTHETICstdout");
            System.out.println();
            System.err.println("Exec3 stderr password=Exec3SYNTHETICstderr");
            System.err.flush();
        }
        System.out.println("Exec3LogTest PASS credentials are redacted in the logcat file and stderr for Java, split native arguments, multi-line and non-ASCII text");
        command("logcat", "-c");
        equal("", command("logcat", "-d", "-v", "raw", "-s", tag));
        if (operator) Exec3AppPropertiesTest.request(phase, "log-verbose");
        else set.invoke(null, "log.tag." + tag, "V");
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
