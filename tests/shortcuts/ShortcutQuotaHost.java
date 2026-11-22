/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class ShortcutQuotaHost {
    private static void work(int expected, java.nio.file.Path receipt) throws Exception {
        // Host diagnostics only: observe the JDK's real FileOutputStream writes.
        // No quota, AppShell, Device or file-I/O implementation is replaced.
        try (var recording = new jdk.jfr.Recording()) {
            recording.enable("jdk.FileWrite").withThreshold(java.time.Duration.ZERO).withoutStackTrace();
            recording.start();
            gesher.services.ShortcutQuotaContractTest.workload(expected);
            recording.stop(); recording.dump(receipt);
        }
        long dynamicWrites = 0, dynamicBytes = 0, quotaWrites = 0, quotaBytes = 0;
        try (var events = new jdk.jfr.consumer.RecordingFile(receipt)) {
            while (events.hasMoreEvents()) {
                var event = events.readEvent();
                if (!event.getEventType().getName().equals("jdk.FileWrite")) continue;
                String path = event.getString("path");
                long bytes = event.getLong("bytesWritten");
                if (path == null || bytes <= 0) continue;
                if (path.contains("/system/dynamic-shortcuts/") && path.endsWith(".properties.tmp")) {
                    dynamicWrites++; dynamicBytes += bytes;
                } else if (path.endsWith("/system/shortcut-quota.bin.new")) {
                    quotaWrites++; quotaBytes += bytes;
                }
            }
        }
        if (dynamicWrites != 2L * expected) throw new AssertionError("Actual metadata writes: " + dynamicWrites);
        if (expected == 10 && (quotaWrites < 1 || quotaWrites > 12)) throw new AssertionError("Quota durability writes: " + quotaWrites);
        if (expected == 10000 && quotaWrites != 0) throw new AssertionError("Baseline acquired candidate quota state");
        System.out.println("SHORTCUT_FILE_WORK {\"dynamic_writes\":" + dynamicWrites + ",\"dynamic_bytes\":" + dynamicBytes
                + ",\"quota_writes\":" + quotaWrites + ",\"quota_bytes\":" + quotaBytes + "}");
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Class<?> locale = Class.forName("com.gesher.app.locale.SystemLocale");
        var locales = locale.getDeclaredField("sLocales"); locales.setAccessible(true);
        locales.set(null, new android.os.LocaleList(java.util.Locale.US));
        var hours = locale.getDeclaredField("sIs24Hour"); hours.setAccessible(true);
        hours.set(null, Boolean.TRUE); // host has no Android ICU resource bundle
        if (args[1].equals("all") || args[1].equals("contracts")) gesher.services.ShortcutQuotaContractTest.contracts(false);
        if (args[1].equals("all") || args[1].equals("work")) work(Integer.parseInt(args[2]), java.nio.file.Path.of(args[3]));
        if (args[1].equals("all") || args[1].equals("graph")) {
            com.sun.management.ThreadMXBean metrics = (com.sun.management.ThreadMXBean)
                    java.lang.management.ManagementFactory.getThreadMXBean();
            metrics.setThreadAllocatedMemoryEnabled(true); metrics.setThreadCpuTimeEnabled(true);
            long thread = Thread.currentThread().threadId();
            gesher.services.ShortcutQuotaContractTest.graphWorkload(Integer.parseInt(args[4]), Integer.parseInt(args[5]),
                    metrics::getCurrentThreadCpuTime, () -> metrics.getThreadAllocatedBytes(thread));
        }
    }
}
