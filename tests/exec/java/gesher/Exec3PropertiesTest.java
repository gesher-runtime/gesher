// One property backend for Java, separate native executables and tools.
package gesher;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.InputStreamReader;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;

public final class Exec3PropertiesTest {
    private static Method get;
    private static Method set;

    private static String property(String name) throws Exception {
        return (String) get.invoke(null, name);
    }

    private static String command(ProcessBuilder builder, int wanted) throws Exception {
        Process process = builder.redirectErrorStream(true).start();
        try {
            process.getOutputStream().close();
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            byte[] buffer = new byte[8192];
            int count;
            while ((count = process.getInputStream().read(buffer)) >= 0) output.write(buffer, 0, count);
            if (!process.waitFor(20, TimeUnit.SECONDS) || process.exitValue() != wanted) {
                throw new AssertionError(builder.command() + ": " + output.toString("UTF-8"));
            }
            return output.toString("UTF-8").replace("\r\n", "\n");
        } finally {
            process.destroyForcibly();
        }
    }

    private static void equal(String wanted, String actual) {
        if (!wanted.equals(actual)) throw new AssertionError("expected <" + wanted + ">, got <" + actual + ">");
    }

    private static String line(BufferedReader input) throws Exception {
        FutureTask<String> task = new FutureTask<>(input::readLine);
        Thread thread = new Thread(task, "property-child-output");
        thread.setDaemon(true);
        thread.start();
        return task.get(20, TimeUnit.SECONDS);
    }

    public static void main(String[] args) throws Exception {
        // Standalone dex runs must register framework natives first. APK runs
        // already did so through Main.bootstrap in the boot class loader.
        if (args.length > 1 && args[1].equals("--load-runtime")) System.loadLibrary("android_runtime");
        Class<?> type = Class.forName("android.os.SystemProperties");
        get = type.getMethod("get", String.class);
        set = type.getMethod("set", String.class, String.class);
        String child = new File(args[0], "libexec2child.so").getPath();
        String suffix = Long.toString(System.nanoTime(), 36).replace('-', 'x');
        String name = "debug.exec3." + suffix;
        set.invoke(null, name, "java value");
        equal("java value\n", command(new ProcessBuilder("getprop", name), 0));
        equal("java value\n", command(new ProcessBuilder(child, "property-get", name), 0));
        command(new ProcessBuilder("setprop", name, "tool value"), 0);
        equal("tool value", property(name));
        command(new ProcessBuilder(child, "property-set", name, "native value"), 0);
        equal("native value", property(name));
        System.out.println("Exec3PropertiesTest PASS Java, getprop/setprop and ELF bionic share live properties");

        Process watch = new ProcessBuilder(child, "property-watch", name).start();
        try {
            BufferedReader input = new BufferedReader(new InputStreamReader(watch.getInputStream(), StandardCharsets.UTF_8));
            equal("property-ready:3", line(input));
            command(new ProcessBuilder("setprop", name, "changed after find"), 0);
            watch.getOutputStream().write('\n');
            watch.getOutputStream().close();
            equal("property-value:changed after find:4", line(input));
            if (!watch.waitFor(20, TimeUnit.SECONDS) || watch.exitValue() != 0) {
                throw new AssertionError("cached ELF property serial did not change");
            }
            System.out.println("Exec3PropertiesTest PASS cached ELF property callbacks and serials observe another process");
        } finally {
            watch.destroyForcibly();
        }

        String readOnly = "ro.exec3." + suffix;
        set.invoke(null, readOnly, "once");
        command(new ProcessBuilder("setprop", readOnly, "twice"), 1);
        command(new ProcessBuilder(child, "property-set", readOnly, "twice"), 1);
        boolean refused = false;
        try {
            set.invoke(null, readOnly, "twice");
        } catch (InvocationTargetException expected) {
            refused = expected.getCause() instanceof RuntimeException;
        }
        if (!refused) throw new AssertionError("Java rewrote a ro property");
        equal("once", property(readOnly));
        set.invoke(null, name, "");
        equal("fallback\n", command(new ProcessBuilder("getprop", name, "fallback"), 0));
        command(new ProcessBuilder("setprop", "bad..name", "value"), 1);
        System.out.println("Exec3PropertiesTest PASS ro properties are write-once and invalid names fail");

        String bootOne = "exec3-test-" + suffix + "-one";
        String bootTwo = "exec3-test-" + suffix + "-two";
        String persistent = "persist.exec3." + suffix;
        ProcessBuilder first = new ProcessBuilder("setprop", name, "volatile");
        first.environment().put("GESHER_BINDER", bootOne);
        command(first, 0);
        first = new ProcessBuilder("setprop", persistent, "persistent");
        first.environment().put("GESHER_BINDER", bootOne);
        command(first, 0);
        ProcessBuilder second = new ProcessBuilder("getprop", name, "fresh-boot");
        second.environment().put("GESHER_BINDER", bootTwo);
        equal("fresh-boot\n", command(second, 0));
        second = new ProcessBuilder("getprop", persistent);
        second.environment().put("GESHER_BINDER", bootTwo);
        equal("persistent\n", command(second, 0));
        System.out.println("Exec3PropertiesTest PASS a new device boot clears volatile properties and loads persist properties");
    }
}
