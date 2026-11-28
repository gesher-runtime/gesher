/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import java.lang.management.ManagementFactory;
import java.util.Arrays;
import gesher.services.PackageVisibilityContractTest;

/** Complete public PMS calls, including validation; setup is outside the measured interval. */
public final class PackageUidLookupWorkHost {
    private interface Work { void run(int iterations) throws Exception; }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        if (args.length == 2 && args[1].equals("--verify-work")) {
            System.out.println("Package UID lookup work passed: "
                    + PackageVisibilityContractTest.verifyUidWork() + " checks");
            return;
        }
        int packages = Integer.parseInt(args[1]);
        String kind = args[2];
        int iterations = Integer.parseInt(args[3]), batch = Integer.parseInt(args[4]);
        boolean first = args.length > 5 && args[5].equals("--first");
        if (first && iterations != 1) throw new IllegalArgumentException("First-query measurements require one call");
        var thread = (com.sun.management.ThreadMXBean)ManagementFactory.getThreadMXBean();
        var process = (com.sun.management.OperatingSystemMXBean)ManagementFactory.getOperatingSystemMXBean();
        if (!thread.isThreadAllocatedMemorySupported() || !thread.isCurrentThreadCpuTimeSupported()) {
            throw new AssertionError("Allocation and CPU measurements are required");
        }
        thread.setThreadAllocatedMemoryEnabled(true); thread.setThreadCpuTimeEnabled(true);
        long caller = Thread.currentThread().getId();
        Work workload;
        if (kind.startsWith("visibility-")) {
            var visibility = new PackageVisibilityContractTest.Workload(packages,
                    kind.substring("visibility-".length()), false);
            workload = visibility::run;
        } else {
            var identity = new PackageVisibilityContractTest.UidWorkload(packages, kind, batch);
            workload = identity::run;
        }
        if (!first) workload.run(Math.max(100, iterations * 2));
        long[] wall = new long[first ? 1 : 7], cpu = new long[wall.length];
        long[] processCpu = new long[wall.length], allocated = new long[wall.length];
        for (int sample = 0; sample < wall.length; sample++) {
            long bytesBefore = thread.getThreadAllocatedBytes(caller);
            long processBefore = process.getProcessCpuTime();
            long cpuBefore = thread.getCurrentThreadCpuTime();
            long start = System.nanoTime();
            workload.run(iterations);
            wall[sample] = System.nanoTime() - start;
            cpu[sample] = thread.getCurrentThreadCpuTime() - cpuBefore;
            processCpu[sample] = process.getProcessCpuTime() - processBefore;
            allocated[sample] = thread.getThreadAllocatedBytes(caller) - bytesBefore;
        }
        System.out.println("{\"kind\":\"" + kind + "\",\"packages\":" + packages
                + ",\"iterations\":" + iterations + ",\"uids\":" + batch + ",\"first\":" + first
                + ",\"wall_ns\":" + Arrays.toString(wall) + ",\"caller_cpu_ns\":" + Arrays.toString(cpu)
                + ",\"process_cpu_ns\":" + Arrays.toString(processCpu)
                + ",\"caller_allocated_bytes\":" + Arrays.toString(allocated) + "}");
    }
}
