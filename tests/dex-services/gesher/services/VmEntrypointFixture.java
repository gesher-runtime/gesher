/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

/** Tests the actual VM entrypoint and a retained child across the test/boot loaders. */
public final class VmEntrypointFixture {
    public static void main(String[] args) throws Exception {
        if (args.length == 1 && args[0].equals("child")) {
            System.out.println("ENTRYPOINT_CHILD actual=1");
            return;
        }
        if (args.length != 2) throw new IllegalArgumentException("expected log and class argument");
        Delegated.run(Path.of(args[0]));
    }

    private static final class Delegated {
        static void run(Path log) throws Exception {
            Class<?> zygote = Class.forName("com.gesher.server.Zygote");
            String vm = (String) zygote.getMethod("vmCommandLineForEntrypoint").invoke(null);
            String command = (String) zygote.getMethod("commandLine", String.class, String.class, List.class)
                    .invoke(null, vm, VmEntrypointFixture.class.getName(), List.of("child"));
            int pid = (Integer) zygote.getMethod("spawn", String.class, List.class, String.class, boolean.class)
                    .invoke(null, command, List.of(), log.toString(), false);
            long status = -2;
            long until = System.nanoTime() + 60_000_000_000L;
            while (status == -2 && System.nanoTime() < until) {
                Thread.sleep(50);
                status = (Long) zygote.getMethod("exitStatus", int.class).invoke(null, pid);
            }
            if (status == -2) {
                zygote.getMethod("kill", int.class).invoke(null, pid);
                throw new AssertionError("owned child timed out");
            }
            if (status != 0 || !Files.readString(log).contains("ENTRYPOINT_CHILD actual=1")) {
                throw new AssertionError("actual child status=" + status);
            }
            if ((Long) zygote.getMethod("exitStatus", int.class).invoke(null, pid) != -1) {
                throw new AssertionError("terminal handle was not consumed");
            }
            System.out.println("ENTRYPOINT_WRAPPER actual_child=1 exit=0 consumed=1");
        }
    }
}
