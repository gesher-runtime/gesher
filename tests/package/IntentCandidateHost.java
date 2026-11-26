/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class IntentCandidateHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        if (args.length > 1 && args[1].startsWith("metadata-")) {
            switch (args[1]) {
                case "metadata-contracts" -> gesher.services.IntentCandidateIndexTest.metadataContracts();
                case "metadata-work" -> gesher.services.IntentCandidateIndexTest.metadataWork();
                case "metadata-coherence" -> gesher.services.IntentCandidateIndexTest.metadataCoherence();
                case "metadata-benchmark" -> metadataBenchmark(Integer.parseInt(args[2]), Integer.parseInt(args[3]),
                        Integer.parseInt(args[4]), args[5]);
                default -> throw new IllegalArgumentException(args[1]);
            }
            return;
        }
        boolean indexed = args.length == 1;
        gesher.services.IntentCandidateIndexTest.contracts(indexed);
        gesher.services.IntentCandidateIndexTest.benchmark(indexed);
        gesher.services.IntentCandidateIndexTest.metadataContracts();
        if (indexed) {
            gesher.services.IntentCandidateIndexTest.metadataWork();
            gesher.services.IntentCandidateIndexTest.metadataCoherence();
        }
        System.out.println("Actual PMS/ApkPackage/Android intent matching and metadata; constructed manifests and host JNI boundaries. No Windows/ART or installer execution claim.");
    }

    private static volatile int consumed;
    private static void metadataBenchmark(int components, int queries, int samples, String scenario) throws Exception {
        if (components < 1 || queries < 1 || samples < 1) throw new IllegalArgumentException("Positive workload sizes required");
        var workload = gesher.services.IntentCandidateIndexTest.metadataWorkload(scenario, components);
        var bean = (com.sun.management.ThreadMXBean)java.lang.management.ManagementFactory.getThreadMXBean();
        if (!bean.isCurrentThreadCpuTimeSupported() || !bean.isThreadAllocatedMemorySupported())
            throw new IllegalStateException("Actual thread CPU/allocation counters required");
        bean.setThreadCpuTimeEnabled(true); bean.setThreadAllocatedMemoryEnabled(true);
        long thread = Thread.currentThread().threadId();
        long coldAllocated = bean.getThreadAllocatedBytes(thread), coldCpu = bean.getCurrentThreadCpuTime();
        long coldStart = System.nanoTime();
        consumed = workload.getAsInt();
        long coldWall = System.nanoTime() - coldStart, coldSpent = bean.getCurrentThreadCpuTime() - coldCpu;
        long coldBytes = bean.getThreadAllocatedBytes(thread) - coldAllocated;
        System.out.println("COMPONENT_METADATA_COLD {\"scenario\":\"" + scenario + "\",\"components\":" + components
                + ",\"cpu_ns\":" + coldSpent + ",\"elapsed_ns\":" + coldWall
                + ",\"allocated_bytes\":" + coldBytes + ",\"checksum\":" + consumed + "}");
        int checksum = 0;
        for (int i = 0; i < Math.max(64, Math.min(100000, queries * 2)); i++) checksum += workload.getAsInt();
        consumed = checksum;
        for (int sample = 0; sample < samples; sample++) {
            System.gc(); // Outside the measured interval; same protocol for each source.
            long allocated = bean.getThreadAllocatedBytes(thread), cpu = bean.getCurrentThreadCpuTime(), time = System.nanoTime();
            checksum = 0;
            for (int q = 0; q < queries; q++) checksum += workload.getAsInt();
            long wall = System.nanoTime() - time, spent = bean.getCurrentThreadCpuTime() - cpu;
            long bytes = bean.getThreadAllocatedBytes(thread) - allocated;
            consumed = checksum;
            System.out.println("COMPONENT_METADATA_SAMPLE {\"scenario\":\"" + scenario + "\",\"components\":" + components
                    + ",\"queries\":" + queries + ",\"sample\":" + sample + ",\"cpu_ns\":" + spent
                    + ",\"elapsed_ns\":" + wall + ",\"allocated_bytes\":" + bytes + ",\"checksum\":" + checksum + "}");
        }
    }
}
