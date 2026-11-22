/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.IApplicationThread;
import android.app.job.IJobScheduler;
import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.app.job.JobWorkItem;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.PermissionInfo;
import android.content.pm.Signature;
import android.os.Binder;
import android.os.Parcel;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedServiceImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Collection;
import java.util.HashMap;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import sun.misc.Unsafe;

/** Real service admission and retirement; native Binder identities remain an explicit host boundary. */
public final class JobSchedulerAdmissionTest {
    private static final String PACKAGE = "gesher.jobscheduler.test";
    private static final String NAMESPACE = "constraint-test";
    private static final int UID = 19842, PID = 789123;
    private static final String LIMIT_MESSAGE = "Apps may not schedule more than 150 distinct jobs";
    private static int checks;
    private static final Unsafe UNSAFE;
    static {
        try { Field f = Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true); UNSAFE = (Unsafe) f.get(null); }
        catch (Exception failure) { throw new ExceptionInInitializerError(failure); }
    }
    private static void check(boolean value, String why) {
        if (!value) throw new AssertionError(why);
        checks++;
    }
    private static Class<?> type(String name) throws Exception { return Class.forName("com.gesher.server." + name); }
    private static Field field(Object object, String name) throws Exception {
        Field result = object.getClass().getDeclaredField(name); result.setAccessible(true); return result;
    }
    private static Object get(Object object, String name) throws Exception { return field(object, name).get(object); }
    private static void set(Object object, String name, Object value) throws Exception { field(object, name).set(object, value); }
    private static Object call(Object object, String name, Class<?>[] types, Object... args) throws Exception {
        Method method = object.getClass().getDeclaredMethod(name, types); method.setAccessible(true);
        try { return method.invoke(object, args); }
        catch (InvocationTargetException failure) {
            if (failure.getCause() instanceof Exception e) throw e;
            if (failure.getCause() instanceof Error e) throw e;
            throw new AssertionError(failure.getCause());
        }
    }
    private static final class Identity implements AutoCloseable {
        final long previous = Binder.clearCallingIdentity();
        Identity(int pid, int uid) {
            Binder.restoreCallingIdentity(((long) uid << 32) | (pid & 0xffffffffL));
            if (Binder.getCallingUid() != uid || Binder.getCallingPid() != pid)
                throw new AssertionError("Fixture identity boundary unavailable");
        }
        @Override public void close() { Binder.restoreCallingIdentity(previous); }
    }
    private static JobInfo info(String owner, int id) {
        return new JobInfo.Builder(id, new ComponentName(owner, owner + ".TestJobService"))
                .setMinimumLatency(3_600_000).build();
    }
    private static JobInfo info(int id) { return info(PACKAGE, id); }
    private static IJobScheduler jobs(JobSchedulerConstraintContractTest.Harness h) { return (IJobScheduler) h.scheduler; }
    @SuppressWarnings("unchecked")
    private static List<Object> records(JobSchedulerConstraintContractTest.Harness h) throws Exception {
        return (List<Object>) get(h.scheduler, "mJobs");
    }
    private static Object find(JobSchedulerConstraintContractTest.Harness h, String pkg, String namespace, int id) throws Exception {
        return call(h.scheduler, "find", new Class<?>[]{String.class, String.class, int.class}, pkg, namespace, id);
    }
    @SuppressWarnings("unchecked")
    private static void accounting(JobSchedulerConstraintContractTest.Harness h) throws Exception {
        synchronized (h.scheduler) {
            Map<Integer, Integer> canonical = new HashMap<>();
            for (Object job : records(h)) {
                int uid = (Integer) get(job, "quotaUid");
                if (uid >= 0) canonical.merge(uid, 1, Integer::sum);
            }
            check(canonical.equals((Map<Integer, Integer>) get(h.scheduler, "mJobsPerUid")),
                    "Admission accounting differs from the actual scheduled registry");
        }
    }
    private interface Operation { void run() throws Exception; }
    private static void limited(Operation operation) throws Exception {
        try { operation.run(); }
        catch (IllegalStateException expected) {
            check(LIMIT_MESSAGE.equals(expected.getMessage()), "Wrong admission error: " + expected);
            return;
        }
        throw new AssertionError("Over-limit job was admitted");
    }

    private static void boundary() throws Exception {
        try (var h = new JobSchedulerConstraintContractTest.Harness(); var identity = new Identity(PID, UID)) {
            IJobScheduler jobs = jobs(h);
            // The pinned Android16 implementation checks >150 before adding. Its
            // public documentation says150; test the actual151 boundary explicitly.
            for (int i = 0; i < 150; i++) check(jobs.schedule("ns" + i % 3, info(i)) == JobScheduler.RESULT_SUCCESS,
                    "Early quota rejection at " + i);
            Object before = find(h, PACKAGE, "ns0", 0);
            check(jobs.schedule("ns0", info(0)) == JobScheduler.RESULT_SUCCESS, "Replacement at150 failed");
            check(records(h).size() == 150 && find(h, PACKAGE, "ns0", 0) != before, "Replacement changed cardinality");
            JobInfo last = new JobInfo.Builder(150, info(150).getService()).setExpedited(true).build();
            check(jobs.schedule("ns0", last) == JobScheduler.RESULT_SUCCESS && records(h).size() == 151,
                    "Pinned151st admission was lost");
            Object retained = find(h, PACKAGE, "ns0", 150);
            limited(() -> jobs.schedule("another-namespace", info(151)));
            limited(() -> jobs.schedule("ns0", last));
            JobWorkItem first = new JobWorkItem(new Intent("first"));
            check(jobs.enqueue("ns0", last, first) == JobScheduler.RESULT_SUCCESS && first.getWorkId() == 1,
                    "Identical-info enqueue fast path was blocked at capacity");
            JobWorkItem rejected = new JobWorkItem(new Intent("rejected"));
            JobInfo changed = new JobInfo.Builder(150, last.getService()).setMinimumLatency(7_200_000).build();
            limited(() -> jobs.enqueue("ns0", changed, rejected));
            check(rejected.getWorkId() == 0 && find(h, PACKAGE, "ns0", 150) == retained
                    && ((Collection<?>) get(retained, "work")).size() == 1, "Rejected work changed the old job/queue");
            try { jobs.schedule("ns0", new JobInfo.Builder(160, new ComponentName(PACKAGE, PACKAGE + ".Missing")).build());
                throw new AssertionError("Missing service accepted at capacity");
            } catch (IllegalArgumentException expected) { checks++; }
            try { jobs.scheduleAsPackage("ns0", last, PACKAGE, 0, "forged");
                throw new AssertionError("Application gained attributed scheduling permission");
            } catch (SecurityException expected) { checks++; }
            jobs.cancel("ns0", 0);
            check(jobs.schedule("after-cancel", info(151)) == JobScheduler.RESULT_SUCCESS, "Cancellation did not free capacity");
            accounting(h);
            jobs.cancelAllInNamespace("ns0");
            accounting(h);
            jobs.cancelAll();
            check(records(h).isEmpty() && ((Map<?, ?>) get(h.scheduler, "mJobsPerUid")).isEmpty(), "Empty registry retained UID state");
            jobs.schedule("same-id-a", info(0)); jobs.schedule("same-id-b", info(0));
            check(records(h).size() == 2, "Namespace-distinct job IDs shared one reservation");
            jobs.cancel("same-id-a", 0);
            check(records(h).size() == 1 && jobs.getPendingJob("same-id-b", 0) != null,
                    "Namespace cancellation retired a different reservation");
            accounting(h); jobs.cancelAll();
        }
    }

    /** Install only fixture metadata; source ownership/permission checks stay in production. */
    private static void owner(JobSchedulerConstraintContractTest.Harness h, String name, int uid, int pid) throws Exception {
        owner(h, name, uid, pid, (byte) 42);
    }
    @SuppressWarnings("unchecked")
    private static void owner(JobSchedulerConstraintContractTest.Harness h, String name, int uid, int pid, byte signer) throws Exception {
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        ParsedServiceImpl service = new ParsedServiceImpl();
        service.setName(name + ".TestJobService"); service.setPackageName(name); service.setProcessName(name);
        service.setEnabled(true); service.setPermission(android.Manifest.permission.BIND_JOB_SERVICE);
        parsed.addService(service); parsed.setEnabled(true); parsed.setTargetSdkVersion(36); parsed.hideAsFinal();
        Object pkg = UNSAFE.allocateInstance(type("ApkPackage"));
        set(pkg, "packageName", name); set(pkg, "mUid", uid); set(pkg, "parsed", parsed);
        set(pkg, "apkPath", "/fixture/" + name + "/base.apk"); set(pkg, "mDataDir", "/fixture/" + name + "/data");
        Object original = call(h.pm, "get", new Class<?>[]{String.class}, PACKAGE);
        set(pkg, "sharedLibraries", get(original, "sharedLibraries"));
        set(pkg, "mSigningIdentityChecked", true); set(pkg, "mVerifiedSignatures", new Signature[]{new Signature(new byte[]{signer})});
        synchronized (h.pm) {
            Map<String, Object> all = new LinkedHashMap<>((Map<String, Object>) get(h.pm, "mPackages"));
            all.put(name, pkg); call(h.pm, "publishPackages", new Class<?>[]{Map.class}, all);
        }
        Class<?> processType = type("ActivityTaskManagerService$ProcessRecord");
        Constructor<?> constructor = processType.getDeclaredConstructor(h.atm.getClass(), int.class, IApplicationThread.class);
        constructor.setAccessible(true);
        Object process = constructor.newInstance(h.atm, pid, new IApplicationThread.Default());
        set(process, "uid", uid); set(process, "packageName", name); set(process, "packageCode", pkg);
        set(process, "processName", name); set(process, "ready", true);
        ((List<Object>) get(h.atm, "mProcesses")).add(process);
    }

    private static void identities() throws Exception {
        try (var h = new JobSchedulerConstraintContractTest.Harness()) {
            IJobScheduler jobs = jobs(h);
            String peer = PACKAGE + ".shared", other = PACKAGE + ".other", system = PACKAGE + ".system";
            owner(h, peer, UID, PID + 1); owner(h, other, UID + 1, PID + 2); owner(h, system, 1000, PID + 3);
            try (var identity = new Identity(PID, UID)) {
                for (int i = 0; i < 151; i++) jobs.schedule("shared-" + i % 3, info(i % 2 == 0 ? PACKAGE : peer, i));
                limited(() -> jobs.schedule("shared-another", info(peer, 999)));
            }
            try (var identity = new Identity(PID + 2, UID + 1)) {
                check(jobs.schedule("other", info(other, 1)) == JobScheduler.RESULT_SUCCESS, "One UID consumed another UID's quota");
            }
            // Replacing a self-scheduled record with attributed work transfers
            // the old reservation, rather than leaking it or charging both UIDs.
            jobs.scheduleAsPackage("shared-0", info(0), PACKAGE, 0, "fixture");
            try (var identity = new Identity(PID, UID)) {
                check(jobs.schedule("released-attribution", info(999)) == JobScheduler.RESULT_SUCCESS,
                        "Attribution replacement retained the old UID's reservation");
                limited(() -> jobs.schedule("released-attribution", info(1000)));
            }
            // System-attributed cross-UID work bypasses admission and is excluded
            // from that system UID's self-scheduled count (JobStore sourceUid rule).
            int systemUid = Binder.getCallingUid();
            for (int i = 0; i < 200; i++) jobs.scheduleAsPackage("attributed", info(2000 + i), PACKAGE, 0, "fixture");
            check(get(find(h, PACKAGE, "attributed", 2000), "quotaUid").equals(-1), "Cross-UID attribution counted as self work");
            for (int i = 0; i < 151; i++) jobs.schedule("system-ordinary", info(3000 + i));
            limited(() -> jobs.schedule("system-ordinary", info(4000)));
            if (systemUid == 1000) {
                check(jobs.scheduleAsPackage("same-uid", info(system, 1), system, 0, "fixture") == JobScheduler.RESULT_SUCCESS,
                        "Same-UID explicit attribution lost its admission exemption");
                check(get(find(h, system, "same-uid", 1), "quotaUid").equals(1000), "Same-UID attributed job disappeared from counting");
            }
            accounting(h);
            call(h.scheduler, "packageRemoved", new Class<?>[]{String.class}, PACKAGE);
            check(records(h).stream().noneMatch(job -> {
                try { return PACKAGE.equals(get(job, "packageName")); }
                catch (Exception error) { throw new AssertionError(error); }
            }), "Package removal retained its own records");
            check(find(h, peer, "shared-1", 1) != null, "Package removal erased its shared UID peer");
            try (var identity = new Identity(PID + 1, UID)) {
                check(jobs.schedule("peer-after-removal", info(peer, 2000)) == JobScheduler.RESULT_SUCCESS,
                        "Package removal did not release only its share of the UID quota");
            }
            accounting(h);
            jobs.cancelAll(); accounting(h);
            check(((Map<?, ?>) get(h.scheduler, "mJobsPerUid")).isEmpty(), "Retired UID counts leaked");
        }
    }

    /** A same-key record left by a replaced owner is retired, not counted twice. */
    private static void superseded() throws Exception {
        try (var h = new JobSchedulerConstraintContractTest.Harness()) {
            IJobScheduler jobs = jobs(h);
            String updated = PACKAGE + ".updated";
            owner(h, updated, UID, PID + 4);
            try (var identity = new Identity(PID + 4, UID)) { jobs.schedule("superseded", info(updated, 1)); }
            Object stale = find(h, updated, "superseded", 1);
            check(stale != null, "Fixture job was not scheduled");
            owner(h, updated, UID, PID + 5, (byte) 43); // Same UID, different signer: old record is no longer current.
            check(find(h, updated, "superseded", 1) == null && records(h).contains(stale), "Fixture record did not become stale");
            try (var identity = new Identity(PID + 5, UID)) { jobs.schedule("superseded", info(updated, 1)); }
            Object replacement = find(h, updated, "superseded", 1);
            check(replacement != null && replacement != stale && !records(h).contains(stale) && records(h).size() == 1,
                    "Stale same-key record was retained beside its replacement");
            check(Integer.valueOf(1).equals(((Map<?, ?>) get(h.scheduler, "mJobsPerUid")).get(UID)),
                    "Replacement of a stale record was counted twice");
            accounting(h);
            try (var identity = new Identity(PID + 5, UID)) { jobs.cancelAll(); }
            check(records(h).isEmpty() && ((Map<?, ?>) get(h.scheduler, "mJobsPerUid")).isEmpty(), "Superseded records leaked quota");
        }
    }

    private static void retirements() throws Exception {
        try (var h = new JobSchedulerConstraintContractTest.Harness(); var identity = new Identity(PID, UID)) {
            IJobScheduler jobs = jobs(h);
            // Real callbacks cover terminal, retry, periodic and obsolete-run paths.
            for (int i = 0; i < 3; i++) jobs.schedule(NAMESPACE, info(5000 + i));
            var completed = h.start(5000, new JobSchedulerConstraintContractTest.TestService());
            completed.params.getCallback().jobFinished(5000, false);
            check(records(h).size() == 2, "Successful completion did not retire admission"); accounting(h);
            completed.params.getCallback().jobFinished(5000, false); accounting(h);
            var retry = h.start(5001, new JobSchedulerConstraintContractTest.TestService());
            retry.params.getCallback().jobFinished(5001, true);
            check(records(h).size() == 2, "Retry lost admission ownership"); accounting(h);
            var replaced = h.start(5002, new JobSchedulerConstraintContractTest.TestService());
            jobs.schedule(NAMESPACE, info(5002));
            replaced.params.getCallback().jobFinished(5002, false);
            check(records(h).size() == 2 && h.getPendingJob(5002) != null, "Old callback retired its replacement"); accounting(h);
            JobInfo periodic = new JobInfo.Builder(5003, info(5003).getService()).setPeriodic(900_000).build();
            jobs.schedule(NAMESPACE, periodic);
            var repeated = h.start(5003, new JobSchedulerConstraintContractTest.TestService());
            repeated.params.getCallback().jobFinished(5003, false);
            check(h.getPendingJob(5003) != null, "Periodic completion lost its admission"); accounting(h);
            call(h.scheduler, "packageRemoved", new Class<?>[]{String.class}, PACKAGE);
            check(records(h).isEmpty() && ((Map<?, ?>) get(h.scheduler, "mJobsPerUid")).isEmpty(), "Package retirement retained quota");
            repeated.params.getCallback().jobFinished(5003, false); accounting(h);
            check(jobs.schedule(NAMESPACE, info(5004)) == JobScheduler.RESULT_SUCCESS, "New removal epoch could not reuse capacity");
            jobs.cancelAll(); accounting(h);
        }
    }

    private static void concurrent() throws Exception {
        try (var h = new JobSchedulerConstraintContractTest.Harness()) {
            IJobScheduler jobs = jobs(h);
            CountDownLatch ready = new CountDownLatch(8), start = new CountDownLatch(1), done = new CountDownLatch(8);
            AtomicReference<Throwable> failure = new AtomicReference<>(); AtomicInteger accepted = new AtomicInteger();
            for (int thread = 0; thread < 8; thread++) {
                final int slot = thread;
                new Thread(() -> {
                    try (var identity = new Identity(PID, UID)) {
                        ready.countDown(); if (!start.await(10, TimeUnit.SECONDS)) throw new AssertionError("Start gate timeout");
                        for (int i = 0; i < 100; i++) {
                            try { jobs.schedule("race-" + slot % 2, info(10_000 + slot * 100 + i)); accepted.incrementAndGet(); }
                            catch (IllegalStateException limit) { if (!LIMIT_MESSAGE.equals(limit.getMessage())) throw limit; }
                        }
                    } catch (Throwable error) { failure.compareAndSet(null, error); }
                    finally { done.countDown(); }
                }, "job-admission-" + thread).start();
            }
            check(ready.await(10, TimeUnit.SECONDS), "Workers did not reach admission gate"); start.countDown();
            check(done.await(20, TimeUnit.SECONDS), "Admission workers did not retire");
            if (failure.get() != null) throw new AssertionError("Concurrent admission failed", failure.get());
            check(accepted.get() == 151 && records(h).size() == 151, "Concurrent admission exceeded or lost capacity"); accounting(h);
            CountDownLatch churn = new CountDownLatch(8);
            for (int thread = 0; thread < 8; thread++) {
                final int slot = thread;
                new Thread(() -> {
                    try (var identity = new Identity(PID, UID)) {
                        for (int i = 0; i < 100; i++) {
                            jobs.cancel("race-" + slot % 2, 10_000 + slot * 100 + i);
                            try { jobs.schedule("churn", info(15_000 + slot * 100 + i)); }
                            catch (IllegalStateException limit) { if (!LIMIT_MESSAGE.equals(limit.getMessage())) throw limit; }
                        }
                    } catch (Throwable error) { failure.compareAndSet(null, error); }
                    finally { churn.countDown(); }
                }, "job-retirement-" + thread).start();
            }
            check(churn.await(20, TimeUnit.SECONDS), "Concurrent retirement workers did not finish");
            if (failure.get() != null) throw new AssertionError("Concurrent retirement failed", failure.get());
            check(records(h).size() <= 151, "Concurrent retirement/admission exceeded the UID bound"); accounting(h);
            try (var identity = new Identity(PID, UID)) { jobs.cancelAll(); }
            check(records(h).isEmpty() && ((Map<?, ?>) get(h.scheduler, "mJobsPerUid")).isEmpty(), "Concurrent records failed retirement");
        }
    }

    @SuppressWarnings("unchecked")
    private static void permitPersistence(JobSchedulerConstraintContractTest.Harness h) throws Exception {
        h.parsed.addUsesPermission(new ParsedUsesPermissionImpl(android.Manifest.permission.RECEIVE_BOOT_COMPLETED, 0, Set.of()));
        Object permission = call(h.pm, "permissions", new Class<?>[0]);
        PermissionInfo definition = new PermissionInfo(); definition.name = android.Manifest.permission.RECEIVE_BOOT_COMPLETED;
        definition.packageName = "android"; definition.protectionLevel = PermissionInfo.PROTECTION_NORMAL;
        ((Map<String, PermissionInfo>) get(get(permission, "mCatalog"), "mFramework")).put(definition.name, definition);
    }

    private static void persistence() throws Exception {
        // This path is required by the real Windows/ART entrypoint. The host
        // adapter only accounts Parcel write sizes and explicitly does not run it.
        try (var h = new JobSchedulerConstraintContractTest.Harness()) {
            permitPersistence(h); IJobScheduler jobs = jobs(h);
            JobInfo normal = new JobInfo.Builder(20_000, info(20_000).getService()).setPersisted(true).setMinimumLatency(3_600_000).build();
            try (var identity = new Identity(PID, UID)) { jobs.schedule("restore", normal); }
            Object ordinary = find(h, PACKAGE, "restore", 20_000);
            JobInfo attributed = new JobInfo.Builder(20_001, normal.getService()).setPersisted(true).setMinimumLatency(3_600_000).build();
            jobs.scheduleAsPackage("restore", attributed, PACKAGE, 0, "fixture");
            JobInfo control = new JobInfo.Builder(20_002, normal.getService()).setPersisted(true).setMinimumLatency(3_600_000).build();
            jobs.schedule("restore", control);
            int controlUid = Binder.getCallingUid();
            File directory = ((File) call(h.scheduler, "persistFile", new Class<?>[]{h.jobType}, ordinary)).getParentFile();
            List<File> legacyFiles = new ArrayList<>();
            try {
                File original = (File) call(h.scheduler, "persistFile", new Class<?>[]{h.jobType}, ordinary);
                byte[] complete = Files.readAllBytes(original.toPath());
                File truncated = new File(directory, "admission-truncated.job");
                Files.write(truncated.toPath(), java.util.Arrays.copyOf(complete, complete.length - 4));
                legacyFiles.add(truncated);
                File malformed = new File(directory, "admission-malformed.job");
                Parcel corrupt = Parcel.obtain();
                try {
                    corrupt.unmarshall(complete, 0, complete.length); corrupt.setDataPosition(complete.length - 4);
                    corrupt.writeInt(-2); Files.write(malformed.toPath(), corrupt.marshall());
                } finally { corrupt.recycle(); }
                legacyFiles.add(malformed);
                // Valid over-limit legacy work must survive upgrade; admission is
                // checked on new requests, not by silently deleting restored work.
                for (int i = 0; i < 152; i++) {
                    JobInfo old = new JobInfo.Builder(21_000 + i, normal.getService()).setPersisted(true).setMinimumLatency(3_600_000).build();
                    Parcel parcel = Parcel.obtain();
                    try {
                        parcel.writeInt(0x474a4232); parcel.writeString(PACKAGE); parcel.writeString("legacy");
                        old.writeToParcel(parcel, 0); parcel.writeInt(UID); parcel.writeString((String) get(ordinary, "ownerIdentity"));
                        File file = new File(directory, "admission-legacy-" + i + ".job");
                        Files.write(file.toPath(), parcel.marshall()); legacyFiles.add(file);
                    } finally { parcel.recycle(); }
                }
                try (var restored = new JobSchedulerConstraintContractTest.Harness()) {
                    permitPersistence(restored);
                    Thread worker;
                    synchronized (restored.scheduler) {
                        set(restored.scheduler, "mThread", null);
                        call(restored.scheduler, "start", new Class<?>[0]);
                        worker = (Thread) get(restored.scheduler, "mThread");
                        worker.interrupt();
                        check(records(restored).size() == 155, "Valid restored records were discarded for admission quota");
                        check(!truncated.exists(), "Truncated attribution metadata was accepted as a new UID");
                        check(!malformed.exists(), "Invalid negative scheduling UID was restored");
                        check(get(find(restored, PACKAGE, "restore", 20_000), "quotaUid").equals(UID), "App scheduling UID changed on restore");
                        check(get(find(restored, PACKAGE, "restore", 20_001), "quotaUid").equals(-1), "Cross-UID attribution changed on restore");
                        check(get(find(restored, PACKAGE, "restore", 20_002), "quotaUid").equals(controlUid), "System ordinary UID changed on restore");
                        accounting(restored);
                    }
                    worker.join(5000); check(!worker.isAlive(), "Restored scheduler worker did not retire");
                    try (var identity = new Identity(PID, UID)) { limited(() -> jobs(restored).schedule("new", info(22_000))); }
                    jobs(restored).cancelAll(); accounting(restored);
                    Method removeListener = type("WindowsHost").getDeclaredMethod("removeListener", type("WindowsHost$Listener"));
                    removeListener.setAccessible(true); removeListener.invoke(null, restored.scheduler);
                }
            } finally {
                jobs.cancelAll(); for (File file : legacyFiles) Files.deleteIfExists(file.toPath());
            }
        }
    }

    /** Count real registry iteration without replacing scheduler behavior or results. */
    private static final class CountingJobs extends ArrayList<Object> {
        long visits;
        @Override public Iterator<Object> iterator() {
            Iterator<Object> actual = super.iterator();
            return new Iterator<>() {
                public boolean hasNext() { return actual.hasNext(); }
                public Object next() { visits++; return actual.next(); }
                public void remove() { actual.remove(); }
            };
        }
    }
    public static void measure(int expectedJobs) throws Exception {
        try (var h = new JobSchedulerConstraintContractTest.Harness(); var identity = new Identity(PID, UID)) {
            IJobScheduler jobs = jobs(h); CountingJobs measured = new CountingJobs(); set(h.scheduler, "mJobs", measured);
            int accepted = 0, rejected = 0;
            for (int i = 0; i < 10_000; i++) {
                try { jobs.schedule("measure", info(30_000 + i)); accepted++; }
                catch (IllegalStateException limit) {
                    if (!LIMIT_MESSAGE.equals(limit.getMessage())) throw limit;
                    rejected++;
                }
            }
            check(accepted == expectedJobs && measured.size() == expectedJobs, "Unexpected retained job count: " + accepted);
            measured.visits = 0;
            for (int i = 0; i < 32; i++) call(h.scheduler, "schedulePass", new Class<?>[]{boolean.class}, false);
            long schedulerVisits = measured.visits;
            measured.visits = 0;
            for (int i = 0; i < 32; i++) check(jobs.getAllPendingJobsInNamespace("measure").getList().size() == expectedJobs,
                    "Pending query lost an admitted job");
            long queryVisits = measured.visits;
            check(schedulerVisits == expectedJobs * 64L && queryVisits == expectedJobs * 32L,
                    "Measured registry iterations changed unexpectedly");
            System.out.println("JOB_ADMISSION_WORK submissions=10000 accepted=" + accepted + " rejected=" + rejected
                    + " retained_jobs=" + measured.size() + " scheduler_passes=32 scheduler_registry_visits=" + schedulerVisits
                    + " pending_queries=32 pending_registry_visits=" + queryVisits);
            jobs.cancelAll(); check(measured.isEmpty(), "Measurement retained scheduled work");
        }
    }
    public static void contracts(boolean realParcel) throws Exception {
        boundary(); identities(); superseded(); retirements(); concurrent();
        if (realParcel) persistence();
        measure(151);
        System.out.println("JobSchedulerAdmissionTest all passed: " + checks + " checks; persisted_parcel=" + realParcel);
    }
}
