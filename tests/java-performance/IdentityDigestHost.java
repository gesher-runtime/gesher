/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import gesher.services.IdentityDigestContractTest;
import com.gesher.server.IdentitySignerHost;
import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Objects;

/** Whole production calls on the JVM; counters observe callers, never replace work. */
public final class IdentityDigestHost {
    private static long sink;
    private static void verify(String result, String expected) {
        if (!Objects.equals(result, expected)) throw new AssertionError("Complete identity/output mismatch: " + result);
        sink += result == null ? 1 : result.length();
    }
    private static void sample(IdentityDigestContractTest.Workload work, String expected,
            String name, String stage, int sample, int iterations,
            com.sun.management.ThreadMXBean metrics) throws Exception {
        long thread = Thread.currentThread().threadId();
        long allocated = metrics.getThreadAllocatedBytes(thread), cpu = metrics.getCurrentThreadCpuTime();
        long start = System.nanoTime();
        for (int i = 0; i < iterations; i++) verify(work.invoke(), expected);
        long elapsed = System.nanoTime() - start;
        cpu = metrics.getCurrentThreadCpuTime() - cpu;
        allocated = metrics.getThreadAllocatedBytes(thread) - allocated;
        if (elapsed <= 0 || cpu <= 0 || allocated < 0) throw new AssertionError("Unavailable caller counters");
        System.out.println("IDENTITY_DIGEST_SAMPLE {\"case\":\"" + name + "\",\"stage\":\"" + stage
                + "\",\"sample\":" + sample + ",\"iterations\":" + iterations + ",\"elapsed_ns\":" + elapsed
                + ",\"caller_cpu_ns\":" + cpu + ",\"caller_allocated_bytes\":" + allocated + "}");
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        // The supplied host API archive lacks Android ICU data and Windows locale APIs.
        Class<?> locale = Class.forName("com.gesher.app.locale.SystemLocale");
        var list = locale.getDeclaredField("sLocales"); list.setAccessible(true);
        list.set(null, new android.os.LocaleList(java.util.Locale.US));
        var hour = locale.getDeclaredField("sIs24Hour"); hour.setAccessible(true); hour.set(null, Boolean.TRUE);
        Path root = Path.of(args[2]); Files.createDirectories(root);
        try {
            if (args[1].equals("contracts")) {
                int checks = IdentityDigestContractTest.contracts(root.resolve("contracts"));
                var publication = IdentityDigestContractTest.prepare("shortcut-publication", root.resolve("publication"), 1);
                try { verify(publication.invoke(), "cert:identity-publication-fixture"); }
                finally { publication.close(); }
                System.out.println("IdentityDigestContractTest all passed: " + checks + " checks; real shortcut publication/readback/retirement passed");
            } else {
                String name = args[1], expected = args[3].equals("<null>") ? null : args[3];
                int warmup = Integer.parseInt(args[4]), iterations = Integer.parseInt(args[5]);
                int samples = Integer.parseInt(args[6]);
                var metrics = (com.sun.management.ThreadMXBean) ManagementFactory.getThreadMXBean();
                metrics.setThreadCpuTimeEnabled(true); metrics.setThreadAllocatedMemoryEnabled(true);
                var work = IdentityDigestContractTest.prepare(name, root, 1 + warmup + samples * iterations,
                        IdentitySignerHost::signer);
                try {
                    sample(work, expected, name, "first", 0, 1, metrics);
                    for (int i = 0; i < warmup; i++) verify(work.invoke(), expected);
                    for (int i = 0; i < samples; i++) sample(work, expected, name, "steady", i, iterations, metrics);
                } finally { work.close(); }
            }
        } finally { IdentityDigestContractTest.deleteOwned(root); }
        System.out.println("IDENTITY_DIGEST_COMPLETE outputs_validated=true owned_inputs_retired=true sink=" + sink);
        System.out.println("Real production hashing/framing, signer cache and file I/O; constructed installed-signature metadata and declared host locale/executable-path boundaries. No APK verification, Windows Shell or ART execution claim.");
    }
}
