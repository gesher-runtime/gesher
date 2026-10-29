/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

/** Exercises the real SystemServer main in the private writer JVM. */
public final class WriterSystemServerFixture {
    public static void main(String[] args) throws Exception {
        if (args.length > 0 && args[0].equals("writer")) {
            String[] serverArgs = new String[args.length - 1];
            System.arraycopy(args, 1, serverArgs, 0, serverArgs.length);
            Class.forName("com.gesher.server.SystemServer").getMethod("main", String[].class)
                    .invoke(null, (Object) serverArgs);
            throw new AssertionError("system server unexpectedly returned");
        }
        if (args.length != 4) throw new IllegalArgumentException("device, logs, and two APKs required");
        String device = args[0];
        Path logs = Path.of(args[1]);
        Files.createDirectories(logs);
        Files.createDirectories(logs.resolve("data"));
        Class<?> z = Class.forName("com.gesher.server.Zygote");
        String exe = (String) z.getMethod("exePath").invoke(null);
        String dir = exe.substring(0, exe.lastIndexOf('\\') + 1);
        String vm = (String) z.getMethod("vmCommandLineForEntrypoint").invoke(null);
        String broker = (String) z.getMethod("quote", String.class).invoke(null, dir + "binderd.exe");
        String manager = (String) z.getMethod("quote", String.class).invoke(null, dir + "servicemanager.exe");
        List<String> writerArgs = new ArrayList<>(List.of("writer", "--persistent", "--idle-timeout", "0",
                args[2], args[3]));
        String writer = (String) z.getMethod("commandLine", String.class, String.class, List.class)
                .invoke(null, vm, WriterSystemServerFixture.class.getName(), writerArgs);
        List<String> paths = List.of(logs.resolve("broker.log").toString(),
                logs.resolve("manager.log").toString(), logs.resolve("server.log").toString());
        for (String path : paths) Files.deleteIfExists(Path.of(path));
        int[] owned = (int[]) z.getMethod("startDevice", List.class, List.class, List.class).invoke(null,
                List.of(broker + " " + device, manager, writer), paths,
                List.of("GESHER_BINDER=" + device, "ANDROID_DATA=" + logs.resolve("data"),
                        "GESHER_LOGS=" + logs.resolve("device-logs")));
        boolean ready = false;
        boolean[] consumed = new boolean[owned.length];
        try {
            long deadline = System.nanoTime() + 120_000_000_000L;
            Path serverLog = logs.resolve("server.log");
            while (System.nanoTime() < deadline) {
                if (Files.exists(serverLog) && Files.readString(serverLog).contains("with servicemanager")) {
                    ready = true;
                    break;
                }
                long status = (Long) z.getMethod("exitStatus", int.class).invoke(null, owned[2]);
                if (status != -2) {
                    consumed[2] = status != -1;
                    throw new AssertionError("writer ended before readiness: " + status);
                }
                Thread.sleep(100);
            }
            if (!ready) throw new AssertionError("writer readiness timed out");
        } finally {
            String cleanupProblem = "";
            for (int i = owned.length - 1; i >= 0; --i) {
                if (consumed[i]) continue;
                long status = (Long) z.getMethod("exitStatus", int.class).invoke(null, owned[i]);
                if (status == -2) {
                    z.getMethod("kill", int.class).invoke(null, owned[i]);
                    long deadline = System.nanoTime() + 5_000_000_000L;
                    do {
                        Thread.sleep(20);
                        status = (Long) z.getMethod("exitStatus", int.class).invoke(null, owned[i]);
                    } while (status == -2 && System.nanoTime() < deadline);
                }
                if (status == -2 || status == -1) cleanupProblem += " pid=" + owned[i] + " status=" + status;
            }
            if (!cleanupProblem.isEmpty()) throw new AssertionError("owned cleanup incomplete:" + cleanupProblem);
        }
        System.out.println("WRITER_SYSTEM_SERVER real_main=1 ready=1 owned_cleanup=1");
    }
}
