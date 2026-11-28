// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
package gesher;

import java.io.*;
import java.lang.reflect.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Properties;
import java.util.concurrent.*;

/** Real app readers and an independently admitted desktop operator writer. */
public final class Exec3AppPropertiesTest {
    private static Method get, set;
    private static String property(String name) throws Exception { return (String)get.invoke(null, name); }
    private static void check(boolean value, String message) { if (!value) throw new AssertionError(message); }
    private static void initialize() throws Exception {
        Class<?> properties = Class.forName("android.os.SystemProperties");
        get = properties.getMethod("get", String.class);
        set = properties.getMethod("set", String.class, String.class);
    }
    private static String read(File file) {
        try { return new String(Files.readAllBytes(file.toPath()), StandardCharsets.US_ASCII); }
        catch (IOException absent) { return ""; }
    }
    private static void write(File file, String value) throws IOException {
        File temporary = new File(file.getPath() + ".tmp");
        Files.write(temporary.toPath(), value.getBytes(StandardCharsets.US_ASCII));
        Files.move(temporary.toPath(), file.toPath(), java.nio.file.StandardCopyOption.ATOMIC_MOVE,
            java.nio.file.StandardCopyOption.REPLACE_EXISTING);
    }
    public static void request(File phase, String stage) throws Exception {
        write(phase, stage);
        File ack = new File(phase.getPath() + ".ack");
        long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(30);
        while (!stage.equals(read(ack)) && System.nanoTime() < end) Thread.sleep(20);
        check(stage.equals(read(ack)), "Operator did not complete phase " + stage);
    }
    private static String command(String... arguments) throws Exception {
        Process process = new ProcessBuilder(arguments).redirectErrorStream(true).start();
        FutureTask<String> output = new FutureTask<>(() -> new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8).replace("\r\n", "\n"));
        Thread reader = new Thread(output, "app-property-output"); reader.setDaemon(true); reader.start();
        try {
            process.getOutputStream().close();
            check(process.waitFor(20, TimeUnit.SECONDS), "Property command timed out");
            String text = output.get(5, TimeUnit.SECONDS);
            check(process.exitValue() == 0, "Property command failed: " + text);
            return text;
        } finally { process.destroyForcibly(); }
    }
    private static void denied(boolean override, String... arguments) throws Exception {
        ProcessBuilder builder = new ProcessBuilder(arguments).redirectErrorStream(true);
        // A child cannot gain writer authority by claiming a privileged UID.
        if (override) {
            builder.environment().put("GESHER_UID", "1000");
            builder.environment().put("GESHER_BINDER_UID", "1000");
        }
        Process process = builder.start();
        FutureTask<byte[]> output = new FutureTask<>(() -> process.getInputStream().readAllBytes());
        Thread reader = new Thread(output, "denied-property-output"); reader.setDaemon(true); reader.start();
        try {
            process.getOutputStream().close();
            check(process.waitFor(20, TimeUnit.SECONDS), "Denied writer timed out");
            output.get(5, TimeUnit.SECONDS);
            check(process.exitValue() != 0, "Application child gained property writer authority");
        } finally { process.destroyForcibly(); }
    }
    private static void validateRun(String run) {
        check(run.matches("[a-z0-9]{8,32}"), "Invalid isolated property run");
    }
    public static String name(String run) { validateRun(run); return "debug.exec3.app." + run; }
    public static String tag(String run) { validateRun(run); return "Exec3App" + run; }
    public static void appCommands() throws Exception {
        check(java.util.regex.Pattern.compile("(?m)^package:.+$").matcher(command("pm", "path", "com.gesher.exec3")).find(), "App could not query its own installed code");
        denied(false, "am", "force-stop", "com.gesher.exec3target");
        System.out.println("Exec3AppPropertiesTest PASS app commands query own package and deny foreign force-stop");
    }

    public static void run(String directory, File phase, String run) throws Exception {
        initialize(); String name = name(run), child = new File(directory, "libexec2child.so").getPath();
        for (String stage : new String[]{"java", "tool", "native"}) {
            request(phase, stage);
            check(stage.equals(property(name)), "Java missed operator " + stage);
            check((stage + "\n").equals(command("getprop", name)), "getprop missed operator " + stage);
            check((stage + "\n").equals(command(child, "property-get", name)), "ELF missed operator " + stage);
        }
        System.out.println("Exec3AppPropertiesTest PASS Java, tool and ELF read real admitted operator writes");
        boolean refused = false;
        try { set.invoke(null, name, "unauthorized"); }
        catch (InvocationTargetException expected) { refused = expected.getCause() instanceof RuntimeException; }
        check(refused, "Application Java property writer was allowed");
        denied(false, "setprop", name, "unauthorized");
        denied(false, child, "property-set", name, "unauthorized");
        denied(true, "setprop", name, "unauthorized");
        denied(true, child, "property-set", name, "unauthorized");
        check("native".equals(property(name)), "Denied application writer changed live property");
        System.out.println("Exec3AppPropertiesTest PASS Java, tool and ELF writes denied despite child UID overrides");
        Process waiter = new ProcessBuilder(child, "property-wait", name).redirectErrorStream(true).start();
        try {
            BufferedReader input = new BufferedReader(new InputStreamReader(waiter.getInputStream(), StandardCharsets.UTF_8));
            FutureTask<String> ready = new FutureTask<>(input::readLine);
            Thread reader = new Thread(ready, "property-wait-ready"); reader.setDaemon(true); reader.start();
            String before = ready.get(20, TimeUnit.SECONDS);
            check(before != null && before.startsWith("property-ready:"), "Real ELF waiter did not become ready");
            request(phase, "wake");
            check(waiter.waitFor(20, TimeUnit.SECONDS), "Operator did not wake app ELF waiter");
            String after = input.readLine();
            check(waiter.exitValue() == 0 && after != null && after.startsWith("property-woke:") &&
                !before.substring(15).equals(after.substring(14)), "Real ELF wait serial did not change");
            check("wake".equals(property(name)), "App did not read live operator update");
        } finally { waiter.destroyForcibly(); }
        System.out.println("Exec3AppPropertiesTest PASS real app ELF waiter observes external operator update");
    }

    private static boolean admitOperator() throws Exception {
        Class<?> device = Class.forName("com.gesher.server.Device");
        File stateFile = (File)device.getMethod("stateFile").invoke(null);
        Properties state = (Properties)device.getMethod("read", File.class).invoke(null, stateFile);
        check(state != null, "Operator requires a live fixture device");
        String broker = (String)device.getField("BINDERD").get(null);
        String server = (String)device.getField("SYSTEM_SERVER").get(null);
        String binding = state.getProperty((String)device.getField("STATE_DEVICE").get(null));
        Class.forName("android.system.Os").getMethod("setenv", String.class, String.class, boolean.class).invoke(null, "GESHER_BINDER", binding, true);
        String root = System.getenv("ANDROID_ROOT");
        Class.forName("com.gesher.app.Main").getMethod("bootstrap", String.class, String.class, String.class)
            .invoke(null, "exec3-property-operator", root + "/build.prop", root + "/framework/framework-res.apk");
        check((Boolean)device.getMethod("alive", Properties.class, String.class).invoke(null, state, broker) &&
              (Boolean)device.getMethod("alive", Properties.class, String.class).invoke(null, state, server), "Stale fixture device");
        check((Integer)Class.forName("android.os.Process").getMethod("myUid").invoke(null) == 2000, "Actual shell operator UID required");
        String protectedKey = (String)device.getField("STATE_PROTECTED").get(null);
        boolean protectedDevice = "true".equals(state.getProperty(protectedKey));
        if (protectedDevice) {
            int pid = (Integer)device.getMethod("pid", Properties.class, String.class).invoke(null, state, broker);
            long birth = (Long)device.getMethod("startTime", Properties.class, String.class).invoke(null, state, broker);
            check((Boolean)Class.forName("com.gesher.server.Zygote").getMethod("nativePrepareProtectedOperator", int.class, long.class)
                .invoke(null, pid, birth), "Actual protected operator admission failed");
        }
        return protectedDevice;
    }
    public static void main(String[] args) throws Exception {
        if (args.length == 1 && args[0].equals("--probe")) {
            System.out.println("Exec3PropertyMode protected=" + admitOperator());
            System.exit(0); return;
        }
        check(args.length == 4 && args[0].equals("--operator"), "Isolated operator arguments required");
        File phase = new File(args[1]); String run = args[2], child = args[3];
        validateRun(run); admitOperator(); initialize();
        check(phase.getParentFile().isDirectory() || phase.getParentFile().mkdirs(), "Operator phase directory unavailable");
        write(new File(phase.getPath() + ".run"), run);
        String previous = ""; long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(220);
        while (System.nanoTime() < end) {
            String stage = read(phase);
            if (stage.equals(previous) || stage.isEmpty()) { Thread.sleep(20); continue; }
            switch (stage) {
                case "java": set.invoke(null, name(run), stage); break;
                case "tool": case "wake": command("setprop", name(run), stage); break;
                case "native": command(child, "property-set", name(run), stage); break;
                case "log-error": set.invoke(null, "log.tag." + tag(run), "E"); break;
                case "log-info": set.invoke(null, "log.tag." + tag(run), "I"); break;
                case "log-verbose": set.invoke(null, "log.tag." + tag(run), "V"); break;
                case "commands": Exec3CommandsTest.main(new String[0]); break;
                case "done": write(new File(phase.getPath() + ".ack"), stage); System.out.println("Exec3AppPropertiesTest operator completed actual phases"); System.exit(0); return;
                default: throw new AssertionError("Invalid fixed operator phase");
            }
            previous = stage; write(new File(phase.getPath() + ".ack"), stage);
        }
        throw new AssertionError("Property operator timed out");
    }
}
