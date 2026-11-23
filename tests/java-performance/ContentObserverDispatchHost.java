/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import java.lang.management.ManagementFactory;
import java.util.Arrays;
import gesher.services.ContentObserverDispatchTest;

/** Measures complete notifyChange calls; no matcher, authorization or callback body is replaced. */
public final class ContentObserverDispatchHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        int registrations = Integer.parseInt(args[1]);
        String kind = args[2];
        int iterations = Integer.parseInt(args[3]);
        var bean = (com.sun.management.ThreadMXBean) ManagementFactory.getThreadMXBean();
        if (!bean.isThreadAllocatedMemorySupported() || !bean.isCurrentThreadCpuTimeSupported()) {
            throw new AssertionError("Allocation and CPU measurements are required for this host benchmark");
        }
        bean.setThreadAllocatedMemoryEnabled(true);
        bean.setThreadCpuTimeEnabled(true);
        long thread = Thread.currentThread().getId();
        long beforeSetup = bean.getThreadAllocatedBytes(thread);
        try (var workload = new ContentObserverDispatchTest.Workload(registrations, kind)) {
            long setupBytes = bean.getThreadAllocatedBytes(thread) - beforeSetup;
            // Warm full dispatch, including URI decoding and callbacks.
            workload.run(workload.matching == registrations ? Math.max(1_000, iterations * 2)
                    : Math.max(20_000, iterations * 2));
            long[] nanos = new long[7], cpuNanos = new long[7], allocated = new long[7];
            for (int sample = 0; sample < nanos.length; sample++) {
                long callsBefore = workload.calls(), urisBefore = workload.uris();
                long bytesBefore = bean.getThreadAllocatedBytes(thread);
                long cpuBefore = bean.getCurrentThreadCpuTime();
                long start = System.nanoTime();
                workload.run(iterations);
                nanos[sample] = System.nanoTime() - start;
                cpuNanos[sample] = bean.getCurrentThreadCpuTime() - cpuBefore;
                allocated[sample] = bean.getThreadAllocatedBytes(thread) - bytesBefore;
                long expected = (long) iterations * workload.matching;
                if (workload.calls() - callsBefore != expected || workload.uris() - urisBefore != expected) {
                    throw new AssertionError("Faster dispatch dropped or duplicated callbacks");
                }
            }
            System.out.println("{\"kind\":\"" + kind + "\",\"registrations\":" + registrations
                    + ",\"iterations\":" + iterations + ",\"callbacks_per_iteration\":" + workload.matching
                    + ",\"setup_allocated_bytes\":" + setupBytes
                    + ",\"wall_ns\":" + Arrays.toString(nanos)
                    + ",\"cpu_ns\":" + Arrays.toString(cpuNanos)
                    + ",\"allocated_bytes\":" + Arrays.toString(allocated) + "}");
        }
    }
}
