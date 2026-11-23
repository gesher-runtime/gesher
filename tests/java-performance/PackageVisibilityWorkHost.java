/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import java.lang.management.ManagementFactory;
import java.util.Arrays;
import gesher.services.PackageVisibilityContractTest;

/** Complete ApplicationPackageManager/PMS queries over ordinary constructed package records. */
public final class PackageVisibilityWorkHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        if (args.length == 2 && args[1].equals("--verify-work")) {
            System.out.println("Package visibility work passed: "
                    + PackageVisibilityContractTest.verifyWork() + " budgets");
            return;
        }
        int packages = Integer.parseInt(args[1]);
        String kind = args[2];
        int iterations = Integer.parseInt(args[3]);
        var bean = (com.sun.management.ThreadMXBean) ManagementFactory.getThreadMXBean();
        if (!bean.isThreadAllocatedMemorySupported() || !bean.isCurrentThreadCpuTimeSupported()) {
            throw new AssertionError("Allocation and CPU measurements are required");
        }
        bean.setThreadAllocatedMemoryEnabled(true);
        bean.setThreadCpuTimeEnabled(true);
        long thread = Thread.currentThread().getId();
        var workload = new PackageVisibilityContractTest.Workload(packages, kind, false);
        workload.run(Math.max(100, iterations * 2));
        long[] wall = new long[7], cpu = new long[7], allocated = new long[7];
        for (int sample = 0; sample < wall.length; sample++) {
            long bytesBefore = bean.getThreadAllocatedBytes(thread);
            long cpuBefore = bean.getCurrentThreadCpuTime();
            long start = System.nanoTime();
            workload.run(iterations);
            wall[sample] = System.nanoTime() - start;
            cpu[sample] = bean.getCurrentThreadCpuTime() - cpuBefore;
            allocated[sample] = bean.getThreadAllocatedBytes(thread) - bytesBefore;
        }
        System.out.println("{\"kind\":\"" + kind + "\",\"packages\":" + packages
                + ",\"iterations\":" + iterations + ",\"visible_per_iteration\":" + workload.expectedVisible
                + ",\"wall_ns\":" + Arrays.toString(wall)
                + ",\"cpu_ns\":" + Arrays.toString(cpu)
                + ",\"allocated_bytes\":" + Arrays.toString(allocated) + "}");
    }
}
