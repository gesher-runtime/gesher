/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.content.ComponentName;
import android.content.ContentResolver;
import android.content.IContentService;
import android.net.Uri;
import android.os.Binder;
import android.os.Parcel;
import android.os.SystemClock;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import sun.misc.Unsafe;

/** Real content-trigger accumulation, eligibility and JobService parameter paths. */
public final class JobSchedulerContentTriggerTest {
    private static final String PACKAGE = "gesher.jobscheduler.test";
    private static final Uri A = Uri.parse("content://trigger-a/rows");
    private static final Uri B = Uri.parse("content://trigger-b/rows");
    private static int checks;
    private static void check(boolean condition, String why) {
        if (!condition) throw new AssertionError(why);
        checks++;
    }
    private static Field field(Object target, String name) throws Exception {
        Field f = target.getClass().getDeclaredField(name); f.setAccessible(true); return f;
    }
    private static Object get(Object target, String name) throws Exception { return field(target, name).get(target); }
    private static void set(Object target, String name, Object value) throws Exception { field(target, name).set(target, value); }
    private static Object call(Object target, String name, Class<?>[] types, Object... args) throws Exception {
        Method m = target.getClass().getDeclaredMethod(name, types); m.setAccessible(true);
        try { return m.invoke(target, args); }
        catch (InvocationTargetException e) {
            if (e.getCause() instanceof Exception failure) throw failure;
            if (e.getCause() instanceof Error failure) throw failure;
            throw new AssertionError(e.getCause());
        }
    }
    private static final class Identity implements AutoCloseable {
        final long before = Binder.clearCallingIdentity();
        Identity() { Binder.restoreCallingIdentity(((long) 19842 << 32) | 789123L); }
        public void close() { Binder.restoreCallingIdentity(before); }
    }
    private static JobInfo.Builder builder(int id, long update, long max, boolean charging, Uri... roots) {
        JobInfo.Builder b = new JobInfo.Builder(id, new ComponentName(PACKAGE, PACKAGE + ".TestJobService"));
        for (Uri root : roots) b.addTriggerContentUri(new JobInfo.TriggerContentUri(root,
                JobInfo.TriggerContentUri.FLAG_NOTIFY_FOR_DESCENDANTS));
        if (update >= 0) b.setTriggerContentUpdateDelay(update);
        if (max >= 0) b.setTriggerContentMaxDelay(max);
        return b.setRequiresCharging(charging);
    }
    private static Uri[] changes(Uri root, int first, int count) {
        Uri[] result = new Uri[count];
        for (int i = 0; i < count; i++) result[i] = Uri.parse(root + "/" + (first + i));
        return result;
    }
    private static Set<Uri> values(Uri[] uris) { return new HashSet<>(Arrays.asList(uris)); }
    @SuppressWarnings("unchecked")
    private static Set<Uri> retained(Object job) throws Exception { return (Set<Uri>) get(job, "changedUris"); }
    @SuppressWarnings("unchecked")
    private static Set<String> authorities(Object job) throws Exception { return (Set<String>) get(job, "changedAuthorities"); }
    private static void notify(JobSchedulerConstraintContractTest.Harness h, int flags, Uri... uris) throws Exception {
        call(h.scheduler, "contentChanged", new Class<?>[]{Uri[].class, int.class}, uris, flags);
    }
    private static boolean hasReason(JobSchedulerConstraintContractTest.Harness h, int id, int reason) throws Exception {
        for (int actual : h.reasons(id)) if (actual == reason) return true;
        return false;
    }
    private static long readyAt(JobSchedulerConstraintContractTest.Harness h, Object job, long now) throws Exception {
        synchronized (h.scheduler) {
            return (Long) call(h.scheduler, "triggerReadyAt", new Class<?>[]{h.jobType, long.class}, job, now);
        }
    }
    private static void delivered(JobParameters params, Set<Uri> uris, Set<String> authorities,
            boolean realParcel) {
        check(params != null && params.getTriggeredContentUris() != null, "Retained trigger URIs were lost");
        check(values(params.getTriggeredContentUris()).equals(uris), "Delivered URI set differs");
        check(new HashSet<>(Arrays.asList(params.getTriggeredContentAuthorities())).equals(authorities),
                "Delivered authority set differs");
        if (realParcel) {
            Parcel parcel = Parcel.obtain();
            try {
                params.writeToParcel(parcel, 0); parcel.setDataPosition(0);
                JobParameters copy = JobParameters.CREATOR.createFromParcel(parcel);
                check(values(copy.getTriggeredContentUris()).equals(uris), "URI Parcel round trip differs");
                check(new HashSet<>(Arrays.asList(copy.getTriggeredContentAuthorities())).equals(authorities),
                        "Authority Parcel round trip differs");
            } finally { parcel.recycle(); }
        }
    }
    private static void freshBoundAndUrgency(boolean realParcel) throws Exception {
        try (var h = new JobSchedulerConstraintContractTest.Harness(); var identity = new Identity()) {
            h.setPowerStatus(new int[]{0, 0, 80});
            int id = 3000;
            h.schedule(builder(id, 60_000, 120_000, true, A, B).build());
            Object job = h.findJob(id);
            notify(h, 0, changes(A, 0, 39));
            check(retained(job).size() == 39, "Distinct 39-URI batch changed");
            check(hasReason(h, id, JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONTENT_TRIGGER), "39 URIs bypassed debounce");
            notify(h, 0, changes(A, 0, 39));
            notify(h, 0, Uri.parse("content://unmatched/rows/0"));
            check(retained(job).size() == 39, "Duplicate or unrelated URI filled the batch");
            check(hasReason(h, id, JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONTENT_TRIGGER), "Duplicates triggered urgent execution");
            notify(h, 0, changes(A, 39, 1));
            check(!hasReason(h, id, JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONTENT_TRIGGER), "40 URIs did not trigger urgently");
            check(hasReason(h, id, JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CHARGING) && h.runningCount() == 0,
                    "Urgent content bypassed the unsatisfied charging constraint");
            notify(h, 0, changes(A, 40, 10));
            check(retained(job).size() == 50, "50th URI was lost");
            notify(h, 0, changes(A, 50, 1));
            check(retained(job).size() == 50, "51st newly observed URI was retained");
            notify(h, 0, changes(A, 51, 1000));
            notify(h, 0, changes(B, 0, 1));
            check(retained(job).equals(values(changes(A, 0, 50))), "Fresh notification batch exceeded or changed its first 50 URIs");
            check(authorities(job).equals(Set.of("trigger-a", "trigger-b")), "URI saturation lost a changed authority");
            h.setPowerStatus(new int[]{1, 0, 100});
            check(!hasReason(h, id, JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CHARGING), "Restored charging remained blocked");
            var run = h.start(id, new JobSchedulerConstraintContractTest.TestService());
            delivered(run.params, values(changes(A, 0, 50)), Set.of("trigger-a", "trigger-b"), realParcel);
            check(retained(job).isEmpty() && authorities(job).isEmpty(), "Run did not take ownership of its pending batch");
            run.params.getCallback().jobFinished(id, false);
            check(h.findJob(id) == null, "Completed trigger job was retained");
        }
    }
    private static void debounceAndLatch() throws Exception {
        try (var h = new JobSchedulerConstraintContractTest.Harness(); var identity = new Identity()) {
            h.setPowerStatus(new int[]{0, 0, 80});
            int id = 3001;
            h.schedule(builder(id, 0, 0, true, A).build());
            notify(h, 0, changes(A, 0, 1)); Object job = h.findJob(id);
            long first = (Long) get(job, "firstChange");
            check(readyAt(h, job, first + 499) == first + 500, "Minimum 500 ms quiet period was ignored");
            check(readyAt(h, job, first + 500) == 0, "Quiet-period boundary did not satisfy content");
            notify(h, 0, changes(A, 1, 1));
            check(readyAt(h, job, first + 501) == 0, "A later change revoked an already satisfied content constraint");
            h.cancel(id);

            h.schedule(builder(id, 500, 0, true, A).build());
            notify(h, 0, changes(A, 0, 1)); job = h.findJob(id); first = (Long) get(job, "firstChange");
            // Controlled elapsed timestamps exercise the real scheduler deadline function;
            // no wall-clock sleep or host timer timing is substituted as Android evidence.
            set(job, "lastChange", first + 900);
            check(readyAt(h, job, first + 999) == first + 1000, "Minimum 1,000 ms maximum delay was ignored");
            check(readyAt(h, job, first + 1000) == 0, "Maximum-delay boundary did not satisfy content");
            h.cancel(id);

            h.schedule(builder(id, -1, -1, true, A).build());
            notify(h, 0, changes(A, 0, 1)); job = h.findJob(id); first = (Long) get(job, "firstChange");
            check(readyAt(h, job, first) == first + 10_000, "Default update delay changed");
            set(job, "lastChange", first + 119_999);
            check(readyAt(h, job, first + 119_999) == first + 120_000, "Default maximum delay changed");
            h.cancel(id);

            h.schedule(builder(id, Long.MAX_VALUE, Long.MAX_VALUE, true, A).build());
            notify(h, 0, changes(A, 0, 1)); job = h.findJob(id);
            check(readyAt(h, job, SystemClock.elapsedRealtime()) == Long.MAX_VALUE, "Long delay overflowed into immediate execution");
            notify(h, 0, changes(A, 1, 39));
            check(readyAt(h, job, SystemClock.elapsedRealtime()) == 0, "Urgency did not override a long debounce");
        }
    }
    private static void retryUnions(boolean realParcel) throws Exception {
        try (var h = new JobSchedulerConstraintContractTest.Harness(); var identity = new Identity()) {
            int id = 3002;
            h.schedule(builder(id, 60_000, 120_000, false, A).build());
            Set<Uri> expected = new HashSet<>();
            notify(h, 0, changes(A, 0, 50)); expected.addAll(values(changes(A, 0, 50)));
            Object job = h.findJob(id);
            for (int attempt = 0; attempt < 3; attempt++) {
                set(job, "earliest", 0L); // Expire only the existing retry-backoff boundary.
                check(!hasReason(h, id, JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONTENT_TRIGGER), "Retry reintroduced content debounce");
                var run = h.start(id, new JobSchedulerConstraintContractTest.TestService());
                delivered(run.params, expected, Set.of("trigger-a"), realParcel);
                if (attempt == 2) { run.params.getCallback().jobFinished(id, false); break; }
                Uri[] next = changes(A, (attempt + 1) * 100, 50);
                notify(h, 0, next); notify(h, 0, changes(A, 9000, 10));
                check(retained(job).equals(values(next)), "Active run's next batch was not independently bounded");
                expected.addAll(values(next));
                run.params.getCallback().jobFinished(id, true);
                check(retained(job).equals(expected), "Retry truncated the union of reported and pending URIs");
            }
            check(expected.size() == 150 && h.findJob(id) == null, "Repeated retry union or completion differed");

            h.schedule(builder(id, 0, 0, false, A).build());
            notify(h, 0, changes(A, 0, 1)); job = h.findJob(id);
            readyAt(h, job, (Long) get(job, "firstChange") + 500);
            var one = h.start(id, new JobSchedulerConstraintContractTest.TestService());
            one.params.getCallback().jobFinished(id, true);
            check(!hasReason(h, id, JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONTENT_TRIGGER), "Single-URI retry was delayed as a new notification");
            h.cancel(id);

            h.schedule(builder(id, 60_000, 120_000, false, A).setOverrideDeadline(0).build());
            var deadline = h.start(id, new JobSchedulerConstraintContractTest.TestService(), true);
            check(deadline.params.getTriggeredContentUris() == null
                    && deadline.params.getTriggeredContentAuthorities() == null, "Deadline-only run fabricated content");
            notify(h, 0, changes(A, 0, 1));
            deadline.params.getCallback().jobFinished(id, true);
            check(!hasReason(h, id, JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONTENT_TRIGGER),
                    "Retry failed to take pending content from a deadline-only run");
        }
    }
    private static void replacementAndRetirement(boolean realParcel) throws Exception {
        try (var h = new JobSchedulerConstraintContractTest.Harness(); var identity = new Identity()) {
            h.setPowerStatus(new int[]{0, 0, 80}); int id = 3003;
            h.schedule(builder(id, 60_000, 120_000, true, A).build());
            notify(h, 0, changes(A, 0, 10)); Object old = h.findJob(id);
            h.schedule(builder(id, 60_000, 120_000, true, B).build()); Object current = h.findJob(id);
            check(current != old && retained(old).isEmpty(), "Replacement did not transfer pending ownership");
            check(retained(current).equals(values(changes(A, 0, 10))), "Changed trigger definition lost an existing pending batch");
            check(!hasReason(h, id, JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONTENT_TRIGGER), "Transferred pending batch was debounced again");
            notify(h, 0, changes(B, 0, 3));
            Set<Uri> first = values(changes(A, 0, 10)); first.addAll(values(changes(B, 0, 3)));
            h.setPowerStatus(new int[]{1, 0, 100});
            var run = h.start(id, new JobSchedulerConstraintContractTest.TestService());
            delivered(run.params, first, Set.of("trigger-a", "trigger-b"), realParcel);
            notify(h, 0, changes(B, 100, 10));
            h.schedule(builder(id, 60_000, 120_000, true, A).build()); current = h.findJob(id);
            check(run.service.stops == 1, "Replacement did not stop the old run");
            check(retained(current).equals(values(changes(B, 100, 10))), "Replacement mixed its pending batch with the old run's delivered batch");
            run.params.getCallback().jobFinished(id, true);
            check(retained(current).equals(values(changes(B, 100, 10))), "Late old-run completion contaminated replacement state");
            var replacement = h.start(id, new JobSchedulerConstraintContractTest.TestService());
            delivered(replacement.params, values(changes(B, 100, 10)), Set.of("trigger-b"), realParcel);
            replacement.params.getCallback().jobFinished(id, false);

            h.schedule(builder(id, 60_000, 120_000, true, A).build()); notify(h, 0, changes(A, 0, 5));
            old = h.findJob(id); h.cancel(id); notify(h, 0, changes(A, 100, 10));
            check(retained(old).size() == 5, "Canceled schedule still consumed notifications");
            h.schedule(builder(id, 60_000, 120_000, true, A).build());
            check(retained(h.findJob(id)).isEmpty(), "Reused canceled job ID inherited old content");
            call(h.scheduler, "packageRemoved", new Class<?>[]{String.class}, PACKAGE);
            notify(h, 0, changes(A, 0, 50));
            check(h.findJob(id) == null && h.runningCount() == 0, "Removed owner still retained trigger work");
        }
    }
    private static void matching() throws Exception {
        try (var h = new JobSchedulerConstraintContractTest.Harness(); var identity = new Identity()) {
            Uri exact = Uri.parse(A + "/item"); int id = 3004;
            h.schedule(new JobInfo.Builder(id, new ComponentName(PACKAGE, PACKAGE + ".TestJobService"))
                    .addTriggerContentUri(new JobInfo.TriggerContentUri(exact, 0)).build());
            Object job = h.findJob(id);
            notify(h, 0, Uri.parse(exact + "/child"), Uri.parse("content://trigger-a/rows2/item"),
                    Uri.parse("http://trigger-a/rows/item"), Uri.parse("content://trigger-b/rows/item"));
            check(retained(job).isEmpty(), "Unmatched URI triggered an exact observer");
            notify(h, ContentResolver.NOTIFY_SKIP_NOTIFY_FOR_DESCENDANTS, A);
            check(retained(job).isEmpty(), "Skip-descendants flag was ignored");
            notify(h, 0, A); notify(h, 0, exact);
            check(retained(job).equals(Set.of(A, exact)), "Parent or exact URI delivery changed");
        }
    }
    private static void concurrentNotifications() throws Exception {
        try (var h = new JobSchedulerConstraintContractTest.Harness(); var identity = new Identity()) {
            h.setPowerStatus(new int[]{0, 0, 80}); int id = 3005;
            JobInfo info = builder(id, 60_000, 120_000, true, A).build(); h.schedule(info);
            CountDownLatch go = new CountDownLatch(1); AtomicReference<Throwable> failure = new AtomicReference<>();
            List<Thread> workers = new ArrayList<>();
            for (int index = 0; index < 5; index++) {
                final int owner = index;
                Thread thread = new Thread(() -> {
                    try (var workerIdentity = new Identity()) {
                        if (!go.await(5, TimeUnit.SECONDS)) throw new AssertionError("Concurrent start timed out");
                        if (owner == 4) for (int i = 0; i < 25; i++) h.schedule(info);
                        else for (int i = 0; i < 500; i++) notify(h, 0, changes(A, owner * 1000 + i, 1));
                    } catch (Throwable error) { failure.compareAndSet(null, error); }
                }, "content-trigger-fixture-" + index);
                workers.add(thread); thread.start();
            }
            go.countDown();
            for (Thread thread : workers) { thread.join(10_000); check(!thread.isAlive(), "Notification/replacement worker did not retire"); }
            if (failure.get() != null) throw new AssertionError("Concurrent notification failure", failure.get());
            Object job = h.findJob(id);
            check(retained(job).size() == 50 && authorities(job).equals(Set.of("trigger-a")), "Concurrent publication lost or exceeded the pending batch");
            check(hasReason(h, id, JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CHARGING) && h.runningCount() == 0,
                    "Concurrent content notifications bypassed charging");
            h.cancel(id); notify(h, 0, changes(A, 10_000, 100));
            check(h.findJob(id) == null, "Late notification restored a canceled schedule");
        }
    }
    private static void workerAndContentService() throws Exception {
        try (var h = new JobSchedulerConstraintContractTest.Harness(); var identity = new Identity()) {
            // Construct the actual ContentService. Only its unused sync backend is
            // metadata-only; these notifications do not request network sync.
            Class<?> syncType = Class.forName("com.gesher.server.SyncManager");
            Field access = Unsafe.class.getDeclaredField("theUnsafe"); access.setAccessible(true);
            Object sync = ((Unsafe) access.get(null)).allocateInstance(syncType);
            set(sync, "mAm", h.am); set(sync, "mPm", h.pm);
            var constructor = Class.forName("com.gesher.server.ContentService").getDeclaredConstructor(syncType);
            constructor.setAccessible(true);
            IContentService content = (IContentService) constructor.newInstance(sync);
            h.setPowerStatus(new int[]{0, 0, 80});
            AtomicReference<Throwable> failure = new AtomicReference<>();
            Thread worker = new Thread(() -> {
                try { call(h.scheduler, "run", new Class<?>[0]); }
                catch (Throwable error) { failure.set(error); }
            }, "content-trigger-scheduler");
            worker.setDaemon(true); worker.start();
            try {
                for (int attempt = 0; attempt < 2; attempt++) {
                    int id = 3006 + attempt;
                    h.schedule(builder(id, attempt == 0 ? 60_000 : 0, 120_000, true, A).build());
                    Object job = h.findJob(id);
                    long limit = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
                    while (true) {
                        synchronized (h.scheduler) {
                            if ((Integer) get(job, "lastPendingReasons") >= 0
                                    && worker.getState() == Thread.State.TIMED_WAITING) break;
                        }
                        if (System.nanoTime() >= limit) throw new AssertionError("Actual scheduler did not enter its constraint wait");
                        Thread.sleep(1);
                    }
                    long system = Binder.clearCallingIdentity();
                    try {
                        content.notifyChange(changes(A, 0, attempt == 0 ? 40 : 1), null, false,
                                0, 0, 36, "android");
                    } finally { Binder.restoreCallingIdentity(system); }
                    limit = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
                    int reasons;
                    while (true) {
                        synchronized (h.scheduler) { reasons = (Integer) get(job, "lastPendingReasons"); }
                        if ((reasons & (1 << JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONTENT_TRIGGER)) == 0) break;
                        if (System.nanoTime() >= limit) throw new AssertionError("ContentService notification did not wake the real scheduler");
                        Thread.sleep(1);
                    }
                    check((reasons & (1 << JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CHARGING)) != 0
                            && h.runningCount() == 0, "Real scheduler bypassed the secondary constraint");
                    check(retained(job).size() == (attempt == 0 ? 40 : 1), "Public content delivery lost the real pending batch");
                    if (attempt == 1) check(SystemClock.elapsedRealtime() - (Long) get(job, "firstChange") >= 500,
                            "Real scheduler executed the quiet-period boundary too early");
                    h.cancel(id);
                }
            } finally { worker.interrupt(); worker.join(5000); }
            check(!worker.isAlive() && failure.get() == null, "Content scheduler worker did not retire cleanly: " + failure.get());
        }
    }
    public static void contracts(boolean realParcel) throws Exception {
        freshBoundAndUrgency(realParcel); debounceAndLatch(); retryUnions(realParcel);
        replacementAndRetirement(realParcel); matching(); concurrentNotifications(); workerAndContentService();
        System.out.println("JobSchedulerContentTriggerTest passed: " + checks + " checks; real_parcel=" + realParcel);
    }
    public static void workload(int expected) throws Exception {
        try (var h = new JobSchedulerConstraintContractTest.Harness(); var identity = new Identity()) {
            h.setPowerStatus(new int[]{0, 0, 80}); int id = 3100, notifications = 100_000;
            h.schedule(builder(id, 60_000, 120_000, true, A).build());
            // Feed the actual production listener while charging remains unsatisfied.
            // Small batches bound fixture input storage independently of retained state.
            for (int first = 0; first < notifications; first += 100) notify(h, 0, changes(A, first, 100));
            Object job = h.findJob(id); int retained = retained(job).size();
            check(retained == expected, "Pending URI resource bound differs: " + retained + " != " + expected);
            check(authorities(job).equals(Set.of("trigger-a")), "Workload authority changed");
            check(hasReason(h, id, JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CHARGING) && h.runningCount() == 0,
                    "Overload workload did not remain blocked on real charging constraint evaluation");
            check(((Collection<?>) get(h.scheduler, "mJobs")).size() == 1, "Workload changed the scheduled job count");
            System.out.println("JOB_CONTENT_TRIGGER_WORK {\"notifications\":" + notifications
                    + ",\"retained_uri_entries\":" + retained + ",\"changed_authorities\":1,\"running_jobs\":0,\"charging_unsatisfied\":true}");
        }
    }
}
