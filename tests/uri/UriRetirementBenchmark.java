/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.ClipData;
import android.content.Intent;
import android.net.Uri;
import android.os.IBinder;
import com.gesher.server.UriRetirementCaller;
import java.lang.management.ManagementFactory;
import java.lang.reflect.Field;
import java.util.ArrayDeque;
import java.util.Map;
import static gesher.services.UriGrantRetirementContractTest.*;

/** Whole service calls with mandatory results/retirement; no measured reference oracle. */
public final class UriRetirementBenchmark {
    private static final com.sun.management.ThreadMXBean THREAD =
            (com.sun.management.ThreadMXBean) ManagementFactory.getThreadMXBean();
    private static final com.sun.management.OperatingSystemMXBean PROCESS =
            (com.sun.management.OperatingSystemMXBean) ManagementFactory.getOperatingSystemMXBean();
    private interface Operation { long run(int iteration) throws Exception; }
    private record Cost(long elapsed, long cpu, long allocation, long processCpu, long checksum) {}

    private static Cost measure(Operation operation, int count) throws Exception {
        long id = Thread.currentThread().getId();
        long process = PROCESS.getProcessCpuTime();
        long allocated = THREAD.getThreadAllocatedBytes(id);
        long cpu = THREAD.getCurrentThreadCpuTime();
        long start = System.nanoTime();
        long checksum = 0;
        for (int i = 0; i < count; i++) checksum += operation.run(i);
        long elapsed = System.nanoTime() - start;
        cpu = THREAD.getCurrentThreadCpuTime() - cpu;
        allocated = THREAD.getThreadAllocatedBytes(id) - allocated;
        process = PROCESS.getProcessCpuTime() - process;
        if (elapsed < 0 || cpu < 0 || allocated < 0 || process < 0) throw new AssertionError("Unavailable timing observer");
        return new Cost(elapsed, cpu, allocated, process, checksum);
    }

    private static void record(String name, String phase, int sample, int count, Cost cost) {
        System.out.println("URI_RETIREMENT_SAMPLE {\"case\":\"" + name + "\",\"phase\":\"" + phase
                + "\",\"sample\":" + sample + ",\"operations\":" + count + ",\"elapsed_ns\":" + cost.elapsed
                + ",\"caller_cpu_ns\":" + cost.cpu + ",\"caller_allocated_bytes\":" + cost.allocation
                + ",\"process_cpu_ns\":" + cost.processCpu + ",\"checksum\":" + cost.checksum + "}");
    }

    private static Intent batch(int count, boolean prefix) {
        Intent intent = new Intent().setData(uri(0)).addFlags(READ | (prefix ? PREFIX : 0));
        if (count > 1) {
            ClipData clip = ClipData.newRawUri("complete URI batch", uri(1));
            for (int i = 2; i < count; i++) clip.addItem(new ClipData.Item(uri(i)));
            intent.setClipData(clip);
        }
        return intent;
    }

    private static int suffix(String name, String prefix) { return Integer.parseInt(name.substring(prefix.length())); }

    @SuppressWarnings("unchecked")
    public static void run(String name) throws Exception {
        initialize(false);
        if (!THREAD.isCurrentThreadCpuTimeSupported() || !THREAD.isThreadAllocatedMemorySupported())
            throw new AssertionError("Required complete-call observers are unavailable");
        THREAD.setThreadCpuTimeEnabled(true);
        THREAD.setThreadAllocatedMemoryEnabled(true);
        try (Fixture f = new Fixture()) {
            Operation operation;
            int operations = 32768, warmups = 131072;
            int expectedTable;
            boolean queryCaller = true;
            IBinder prefixOwner = null;
            if (name.startsWith("owned_cycle")) {
                int count = suffix(name, "owned_cycle");
                Intent intent = batch(count, false);
                Uri[] uris = new Uri[count];
                for (int i = 0; i < count; i++) uris[i] = uri(i);
                operations = count == 1 ? 2048 : count == 128 ? 32 : 8;
                warmups = count == 1 ? 8192 : count == 128 ? 128 : 32;
                expectedTable = 0;
                queryCaller = false;
                operation = iteration -> {
                    IBinder[] owner = new IBinder[1];
                    f.as(OWNER, () -> owner[0] = UriRetirementCaller.publish(f.grants,
                            f.records.get(OWNER).uid(), CLIENT, intent));
                    try {
                        f.as(CLIENT, () -> {
                            for (Uri value : uris) if (!f.explicit(CLIENT, value, READ))
                                throw new AssertionError("Complete batch lost a requested URI");
                        });
                    } finally { UriRetirementCaller.retire(f.grants, owner[0]); }
                    return count;
                };
            } else if (name.equals("publish_registry")) {
                f.seed(CLIENT, 128);
                ArrayDeque<Runnable> callbacks = (ArrayDeque<Runnable>) get(f.harness, "packageEvents");
                expectedTable = 128;
                operations = 2048;
                warmups = 8192;
                queryCaller = false;
                operation = iteration -> {
                    UriRetirementCaller.publishPackage(f.pm, f.records.get(OWNER));
                    if (callbacks.size() != 1) throw new AssertionError("Publication did not own exactly one callback");
                    callbacks.removeFirst().run();
                    if (!callbacks.isEmpty()) throw new AssertionError("Publication callback did not retire");
                    return 1;
                };
            } else {
                int count;
                boolean expected = true;
                Uri[] values;
                if (name.startsWith("exact")) {
                    count = suffix(name, "exact");
                    f.seed(CLIENT, count);
                    values = new Uri[count];
                    for (int i = 0; i < count; i++) values[i] = uri(i);
                } else if (name.equals("prefix128")) {
                    count = 128;
                    operations = 8192;
                    warmups = 32768;
                    IBinder[] owner = new IBinder[1];
                    f.as(OWNER, () -> owner[0] = UriRetirementCaller.publish(f.grants,
                            f.records.get(OWNER).uid(), CLIENT, batch(128, true)));
                    prefixOwner = owner[0];
                    values = new Uri[]{uri(127).buildUpon().appendPath("child").build()};
                } else if (name.equals("missing128")) {
                    count = 128; f.seed(CLIENT, count);
                    values = new Uri[]{uri(9999)}; expected = false;
                } else if (name.equals("empty_control")) {
                    count = 0; values = new Uri[]{uri(0)}; expected = false;
                } else if (name.equals("hosted1") || name.equals("mixed_hosted128")) {
                    int installed = name.equals("hosted1") ? 0 : 128;
                    f.seed(CLIENT, installed);
                    Object hosted = f.hosted("fixture.timing.hosted");
                    Uri hostedUri = Uri.parse("content://fixture.timing.hosted/item");
                    f.hostedGrant(hosted, CLIENT, hostedUri);
                    values = new Uri[]{hostedUri}; count = installed + 1;
                } else throw new IllegalArgumentException("Unknown fixed workload: " + name);
                expectedTable = count;
                final boolean want = expected;
                operation = iteration -> {
                    boolean actual = f.explicit(CLIENT, values[iteration % values.length], READ);
                    if (actual != want) throw new AssertionError("Complete query returned an unexpected permission result");
                    return actual ? 1 : 0;
                };
            }
            final int calls = operations, warm = warmups;
            Action measurements = () -> {
                record(name, "first", 0, 1, measure(operation, 1));
                for (int i = 0; i < warm; i++) operation.run(i);
                for (int sample = 0; sample < 3; sample++)
                    record(name, "steady", sample, calls, measure(operation, calls));
            };
            if (queryCaller) f.as(CLIENT, measurements); else measurements.run();
            check(f.grantCount(CLIENT) == expectedTable, "Final permission-table ownership changed");
            if (name.equals("publish_registry"))
                f.as(CLIENT, () -> check(f.explicit(CLIENT, uri(0), READ), "Registry publication changed a live grant"));
            if (prefixOwner != null) UriRetirementCaller.retire(f.grants, prefixOwner);
        }
        System.out.println("URI_RETIREMENT_BENCH_COMPLETE case=" + name
                + " complete_results=true real_owner_retirement=true reference_inside_timing=false");
    }

    public static void storage() throws Exception {
        initialize(false);
        try (Fixture f = new Fixture()) {
            f.seed(CLIENT, 128); f.seed(OTHER, 128);
            storagePhase(f, "shared_generation");
            publishStorageGeneration(f);
            f.as(CLIENT, () -> check(f.explicit(CLIENT, uri(0), READ), "Fresh publication lost a live grant"));
            storagePhase(f, "one_table_refreshed");
            publishStorageGeneration(f);
            storagePhase(f, "distinct_table_and_current_generations");
            call(f.grants, "clear", new Class<?>[]{String.class}, (Object) null);
            check(((Map<?, ?>) get(f.grants, "mGrants")).isEmpty(), "Cleared tables retained publication state");
            storagePhase(f, "tables_retired");
        }
    }

    @SuppressWarnings("unchecked")
    private static void publishStorageGeneration(Fixture f) throws Exception {
        ArrayDeque<Runnable> callbacks = (ArrayDeque<Runnable>) get(f.harness, "packageEvents");
        UriRetirementCaller.publishPackage(f.pm, f.records.get(OWNER));
        check(callbacks.size() == 1, "Storage publication did not own exactly one callback");
        callbacks.removeFirst().run();
        check(callbacks.isEmpty(), "Storage publication callback did not retire");
    }

    private static void storagePhase(Fixture f, String phase) throws Exception {
        Map<?, ?> tables = (Map<?, ?>) get(f.grants, "mGrants");
        checkPrimitiveGeneration(f.pm, "mPackageGeneration");
        long tableBytes = 0;
        for (Object table : tables.values()) {
            checkPrimitiveGeneration(table, "retiredPackageGeneration");
            tableBytes += UriRetirementSizeAgent.size(table);
        }
        System.out.println("URI_RETIREMENT_STORAGE {\"phase\":\"" + phase
                + "\",\"pm_bytes\":" + UriRetirementSizeAgent.size(f.pm)
                + ",\"tables\":" + tables.size() + ",\"table_bytes\":" + tableBytes + "}");
    }

    private static void checkPrimitiveGeneration(Object owner, String name) {
        Field field;
        try {
            field = owner.getClass().getDeclaredField(name);
        } catch (NoSuchFieldException baseline) {
            return;
        }
        check(field.getType() == long.class, name + " must not retain an object");
    }
}
