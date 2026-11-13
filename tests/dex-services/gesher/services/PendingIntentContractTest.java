/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.ActivityManager;
import android.app.ActivityThread;
import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.IIntentReceiver;
import android.content.IIntentSender;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.Signature;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;
import com.android.internal.os.IResultReceiver;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import sun.misc.Unsafe;

/** Real PendingIntent/framework/controller contracts; constructed package/process state only. */
public final class PendingIntentContractTest {
    private static final String CREATOR = "gesher.pending.creator", HOLDER = "gesher.pending.holder";
    private static final String ACTION = "gesher.pending.ACTION";
    private static final int MUTABLE = PendingIntent.FLAG_MUTABLE;
    private static final Unsafe UNSAFE;
    public static int checks;
    static {
        try { Field f = Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true); UNSAFE = (Unsafe)f.get(null); }
        catch (Exception failure) { throw new ExceptionInInitializerError(failure); }
    }
    private interface Work { void run() throws Exception; }
    private static synchronized void check(boolean value, String why) {
        if (!value) throw new AssertionError(why);
        checks++;
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name); f.setAccessible(true); return f;
    }
    private static Object get(Object target, String name) throws Exception { return field(target.getClass(), name).get(target); }
    private static void set(Object target, String name, Object value) throws Exception { field(target.getClass(), name).set(target, value); }
    private static Class<?> type(String name) throws Exception { return Class.forName("com.gesher.server." + name); }
    private static Object make(Class<?> type, Class<?>[] parameters, Object... arguments) throws Exception {
        Constructor<?> c = type.getDeclaredConstructor(parameters); c.setAccessible(true);
        try { return c.newInstance(arguments); }
        catch (InvocationTargetException failure) { return rethrow(failure.getCause()); }
    }
    private static Object invoke(Object target, String name, Class<?>[] parameters, Object... arguments) throws Exception {
        Method m = target.getClass().getDeclaredMethod(name, parameters); m.setAccessible(true);
        try { return m.invoke(target, arguments); }
        catch (InvocationTargetException failure) { return rethrow(failure.getCause()); }
    }
    private static Object rethrow(Throwable failure) throws Exception {
        if (failure instanceof Error error) throw error;
        if (failure instanceof Exception exception) throw exception;
        throw new AssertionError(failure);
    }
    private static void rejects(Class<? extends Throwable> expected, Work work) throws Exception {
        try { work.run(); }
        catch (Throwable failure) { check(expected.isInstance(failure), "Wrong failure: " + failure); return; }
        throw new AssertionError("Expected " + expected.getSimpleName());
    }
    private static Bundle bundle(int value) { Bundle b = new Bundle(); b.putInt("value", value); return b; }

    private static final class Receiver extends IIntentReceiver.Stub {
        final AtomicInteger calls = new AtomicInteger();
        final CountDownLatch received = new CountDownLatch(1);
        Intent intent; int code; String data; Bundle extras; boolean ordered;
        Runnable action;
        @Override public void performReceive(Intent i, int c, String d, Bundle e, boolean o, boolean sticky, int user) {
            intent = i; code = c; data = d; extras = e; ordered = o; calls.incrementAndGet();
            if (action != null) action.run(); received.countDown();
        }
    }
    private static final class AppThread extends IApplicationThread.Default {
        final IBinder binder = new Binder();
        int pid, uid;
        volatile int sendingUid; volatile String sendingPackage; volatile int exits;
        @Override public IBinder asBinder() { return binder; }
        @Override public void scheduleRegisteredReceiver(IIntentReceiver receiver, Intent intent,
                int code, String data, Bundle extras, boolean ordered, boolean sticky, boolean assumed,
                int user, int processState, int uid, String packageName) throws RemoteException {
            sendingUid = uid; sendingPackage = packageName;
            long previous = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long)this.uid << 32) | pid);
                receiver.performReceive(intent, code, data, extras, ordered, sticky, user);
            } finally { Binder.restoreCallingIdentity(previous); }
        }
        @Override public void scheduleExit() { exits++; }
    }
    private static final class Cancelled implements IResultReceiver {
        final PowerWakeLockCallbackTest.Token token = new PowerWakeLockCallbackTest.Token();
        final AtomicInteger calls = new AtomicInteger();
        final CountDownLatch received = new CountDownLatch(1);
        Runnable action;
        @Override public IBinder asBinder() { return token.binder; }
        @Override public void send(int code, Bundle data) {
            check(code == 0 && data == null, "Wrong cancellation result");
            calls.incrementAndGet();
            if (action != null) action.run(); received.countDown();
        }
    }

    private static Object code(String name, int uid, int signer) throws Exception {
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(36); parsed.hideAsFinal();
        Object code = UNSAFE.allocateInstance(type("ApkPackage"));
        set(code, "packageName", name); set(code, "mUid", uid); set(code, "parsed", parsed);
        set(code, "apkPath", "/fixture/" + name + "/base.apk"); set(code, "mDataDir", "/fixture/" + name + "/data");
        set(code, "sharedLibraries", field(type("SharedLibraryGraph"), "EMPTY").get(null));
        set(code, "mSigningIdentityChecked", true);
        set(code, "mVerifiedSignatures", new Signature[]{new Signature(new byte[]{(byte)signer})});
        return code;
    }

    private static final class Harness implements AutoCloseable {
        final Object pm, atm, pending, broadcasts, creator, ownerProcess, holderProcess;
        final IActivityManager am;
        final AppThread ownerThread = new AppThread(), holderThread = new AppThread();
        final Class<?> processType;
        final int ownerPid = 873001, holderPid = 873002;
        Harness() throws Exception { this(60000); }
        Harness(long timeout) throws Exception {
            Class<?> pmType = type("PackageManagerService"), atmType = type("ActivityTaskManagerService");
            creator = code(CREATOR, 12351, 1);
            pm = make(pmType, new Class<?>[]{Collection.class}, List.of(creator, code(HOLDER, 12352, 2)));
            atm = make(atmType, new Class<?>[]{pmType}, pm);
            // Omit unrelated settings/ICU initialization. Every exercised service,
            // PendingIntent record, broadcast queue and framework value stays real.
            Class<?> amType = type("ActivityManagerService"), callersType = type("CallerIdentity");
            am = (IActivityManager)UNSAFE.allocateInstance(amType);
            Object callers = make(callersType, new Class<?>[]{pmType, atmType}, pm, atm);
            set(am, "mPm", pm); set(am, "mAtm", atm); set(am, "mCallers", callers);
            Object permissions = invoke(pm, "permissions", new Class<?>[0]);
            set(am, "mPermissions", permissions); invoke(permissions, "attach", new Class<?>[]{callersType}, callers);
            // Synthetic framework manifest metadata; preserve the real protected
            // broadcast policy without requiring unrelated AssetManager/ICU natives.
            Object catalog = get(permissions, "mCatalog");
            set(catalog, "mLoaded", true); set(catalog, "mFrameworkRead", true);
            ((java.util.Set<String>)get(catalog, "mProtectedBroadcasts")).add(ACTION + ".PROTECTED");
            set(am, "mServices", make(type("ActiveServices"), new Class<?>[]{atmType, pmType,
                    type("NotificationManagerService")}, atm, pm, null));
            Object providers = UNSAFE.allocateInstance(type("ContentProviders"));
            for (String map : new String[]{"mPublished", "mPublishedBy", "mClients"}) set(providers, map, new LinkedHashMap<>());
            set(am, "mProviders", providers);
            broadcasts = make(type("BroadcastController"), new Class<?>[]{atmType, pmType, long.class}, atm, pm, timeout);
            set(am, "mBroadcasts", broadcasts);
            pending = make(type("PendingIntents"), new Class<?>[]{amType, pmType}, am, pm);
            set(am, "mPendingIntents", pending);
            invoke(atm, "setActivityManager", new Class<?>[]{amType}, am);
            processType = type("ActivityTaskManagerService$ProcessRecord");
            ownerProcess = process(CREATOR, 12351, ownerPid, ownerThread);
            holderProcess = process(HOLDER, 12352, holderPid, holderThread);
        }
        @SuppressWarnings("unchecked") private Object process(String name, int uid, int pid, AppThread thread) throws Exception {
            thread.uid = uid; thread.pid = pid;
            Object process = make(processType, new Class<?>[]{atm.getClass(), int.class, IApplicationThread.class}, atm, pid, thread);
            set(process, "uid", uid); set(process, "packageName", name); set(process, "processName", name); set(process, "ready", true);
            ((List<Object>)get(atm, "mProcesses")).add(process);
            // A constructed activity record keeps this synthetic process alive; no activity lifecycle is claimed.
            ((List<Object>)get(process, "activities")).add(UNSAFE.allocateInstance(type("ActivityTaskManagerService$ActivityRecord")));
            return process;
        }
        IIntentSender create(int request, Intent intent, String resolvedType, int flags) throws Exception {
            return create(ActivityManager.INTENT_SENDER_BROADCAST, request, new Intent[]{intent}, new String[]{resolvedType}, flags);
        }
        IIntentSender create(int kind, int request, Intent[] intents, String[] types, int flags) throws Exception {
            return am.getIntentSenderWithFeature(kind, CREATOR, null, null, null, request, intents, types, flags, null, 0);
        }
        int send(IIntentSender sender, int code, Intent fill, String resolvedType, Receiver finished) throws Exception {
            return am.sendIntentSender(null, sender, null, code, fill, resolvedType, finished, null, null);
        }
        Receiver receiver(int priority, Runnable action) throws Exception {
            return receiver(false, Context.RECEIVER_EXPORTED, null, priority, action);
        }
        Receiver receiver(boolean holder, int flags, String permission, int priority, Runnable action) throws Exception {
            Receiver receiver = new Receiver(); receiver.action = action;
            IntentFilter filter = new IntentFilter(ACTION); filter.setPriority(priority);
            invoke(broadcasts, "registerReceiver", new Class<?>[]{IApplicationThread.class, String.class,
                    IIntentReceiver.class, IntentFilter.class, int.class, String.class, int.class},
                    holder ? holderThread : ownerThread, holder ? HOLDER : CREATOR,
                    receiver, filter, holder ? holderPid : ownerPid, permission, flags);
            return receiver;
        }
        void finish(int code, String data, Bundle extras) {
            try { asOwner(() -> am.finishReceiver(ownerThread.binder, code, data, extras, false, 0)); }
            catch (Exception failure) { throw new AssertionError(failure); }
        }
        void flush() throws Exception {
            // Baseline has no executor because it never stores any cancellation listeners.
            try { ((ExecutorService)get(pending, "mCancelCallbacks")).submit(() -> {}).get(5, TimeUnit.SECONDS); }
            catch (NoSuchFieldException baseline) { }
        }
        void asHolder(Work work) throws Exception {
            as(12352, holderPid, work);
        }
        void asOwner(Work work) throws Exception {
            as(12351, ownerPid, work);
        }
        private void as(int uid, int pid, Work work) throws Exception {
            long identity = Binder.clearCallingIdentity();
            try { Binder.restoreCallingIdentity(((long)uid << 32) | pid); work.run(); }
            finally { Binder.restoreCallingIdentity(identity); }
        }
        @SuppressWarnings("unchecked") void replaceCreator(int signer) throws Exception {
            Map<String,Object> values = new LinkedHashMap<>((Map<String,Object>)get(pm, "mPackages"));
            values.put(CREATOR, code(CREATOR, 12351, signer)); set(pm, "mPackages", values);
        }
        @Override public void close() throws Exception {
            ((ExecutorService)get(broadcasts, "mTimeouts")).shutdownNow();
            ((ExecutorService)get(get(am, "mServices"), "mRestarts")).shutdownNow();
            ((ExecutorService)get(pm, "mPackageEventExecutor")).shutdownNow();
            try { ((ExecutorService)get(pending, "mCancelCallbacks")).shutdownNow(); }
            catch (NoSuchFieldException baseline) { }
        }
    }

    private static void identity() throws Exception {
        try (Harness h = new Harness()) {
            Intent base = new Intent(ACTION);
            IIntentSender plain = h.create(1, base, "text/plain", MUTABLE);
            IIntentSender image = h.create(1, base, "image/png", MUTABLE);
            check(plain != image, "Resolved MIME type was omitted from PendingIntent identity");
            IIntentSender action = h.create(1, base, "text/plain", MUTABLE | Intent.FILL_IN_ACTION);
            IIntentSender data = h.create(1, base, "text/plain", MUTABLE | Intent.FILL_IN_DATA);
            check(plain != action && action != data, "Persistent fill-in flags alias one token");
            check(h.create(1, base, "text/plain", MUTABLE | PendingIntent.FLAG_NO_CREATE) == plain,
                    "NO_CREATE became part of persistent identity");
            check(h.create(1, base, "text/plain", MUTABLE | PendingIntent.FLAG_UPDATE_CURRENT) == plain,
                    "UPDATE_CURRENT became part of persistent identity");
            check(h.create(1, base, "text/plain", 0) != plain, "Legacy mutable token aliases explicit MUTABLE");
            check(h.create(1, base, "application/missing", MUTABLE | PendingIntent.FLAG_NO_CREATE) == null,
                    "NO_CREATE found the wrong resolved type");
            IIntentSender replacement = h.create(1, base, "image/png", MUTABLE | PendingIntent.FLAG_CANCEL_CURRENT);
            check(replacement != image && h.create(1, base, "text/plain", MUTABLE) == plain,
                    "CANCEL_CURRENT retired the wrong resolved-type key");
            check(h.send(image, 0, null, null, null) == ActivityManager.START_CANCELED, "Superseded token still sends");
            rejects(SecurityException.class, () -> h.asHolder(() -> h.am.cancelIntentSender(plain)));
        }
    }

    private record Delivered(Intent intent, String type) { }
    private static Delivered fill(Harness h, IIntentSender sender, Intent input, String type) throws Exception {
        Class<?> launchType = Class.forName("com.gesher.server.PendingIntents$ActivityLaunch");
        AtomicReference<Delivered> delivered = new AtomicReference<>();
        Object launch = Proxy.newProxyInstance(launchType.getClassLoader(), new Class<?>[]{launchType}, (self, method, args) -> {
            delivered.set(new Delivered((Intent)args[1], (String)args[2])); return 0;
        });
        int result = (Integer)invoke(h.pending, "sendActivity", new Class<?>[]{IIntentSender.class, Intent.class,
                String.class, Bundle.class, launchType}, sender, input, type, null, launch);
        check(result == 0 && delivered.get() != null, "Activity dispatch seam was not reached");
        return delivered.get();
    }
    private static void fillIn() throws Exception {
        try (Harness h = new Harness()) {
            Intent original = new Intent(ACTION).putExtra("creator", "kept");
            IIntentSender absent = h.create(ActivityManager.INTENT_SENDER_ACTIVITY, 2,
                    new Intent[]{original}, new String[]{null}, MUTABLE);
            Intent input = new Intent("replacement").setData(Uri.parse("content://fixture/new")).putExtra("sender", "value");
            Delivered first = fill(h, absent, input, "image/png");
            check("image/png".equals(first.type()) && input.getData().equals(first.intent().getData()),
                    "Filled data lost the sender's resolved MIME type");
            check(ACTION.equals(first.intent().getAction()) && "kept".equals(first.intent().getStringExtra("creator"))
                    && "value".equals(first.intent().getStringExtra("sender")), "Default fill-in action/extras semantics changed");
            Intent fixed = new Intent(ACTION).setData(Uri.parse("content://fixture/original"));
            IIntentSender keep = h.create(ActivityManager.INTENT_SENDER_ACTIVITY, 3, new Intent[]{fixed}, new String[]{"text/plain"}, MUTABLE);
            Delivered kept = fill(h, keep, input, "image/png");
            check(fixed.getData().equals(kept.intent().getData()) && "text/plain".equals(kept.type()), "Unchanged data adopted sender MIME type");
            IIntentSender overwrite = h.create(ActivityManager.INTENT_SENDER_ACTIVITY, 3, new Intent[]{fixed},
                    new String[]{"text/plain"}, MUTABLE | Intent.FILL_IN_ACTION | Intent.FILL_IN_DATA);
            Delivered replaced = fill(h, overwrite, input, "image/png");
            check("replacement".equals(replaced.intent().getAction()) && input.getData().equals(replaced.intent().getData())
                    && "image/png".equals(replaced.type()), "Creator fill-in override flags were ignored");
            check(fill(h, overwrite, input, null).type() == null, "Null sender MIME type was replaced despite data change");
            IIntentSender immutable = h.create(ActivityManager.INTENT_SENDER_ACTIVITY, 4, new Intent[]{fixed},
                    new String[]{"text/plain"}, PendingIntent.FLAG_IMMUTABLE | Intent.FILL_IN_DATA);
            Delivered frozen = fill(h, immutable, input, "image/png");
            check(fixed.getData().equals(frozen.intent().getData()) && "text/plain".equals(frozen.type())
                    && !frozen.intent().hasExtra("sender"), "Immutable token accepted sender data/extras");
            Delivered next = fill(h, absent, null, "wrong/type");
            check(next.intent().getData() == null && next.type() == null && !next.intent().hasExtra("sender"),
                    "Send mutated the stored intent or adopted an unused sender type");
            AtomicReference<Throwable> failed = new AtomicReference<>(); List<Thread> threads = new ArrayList<>();
            for (int i = 0; i < 2; i++) {
                final int marker = i;
                threads.add(new Thread(() -> { try {
                    Delivered d = fill(h, absent, new Intent().putExtra("parallel", marker), null);
                    check(d.intent().getIntExtra("parallel", -1) == marker, "Parallel sends shared an intent");
                } catch (Throwable failure) { failed.set(failure); } }));
            }
            for (Thread thread : threads) thread.start();
            for (Thread thread : threads) thread.join(5000);
            check(failed.get() == null && threads.stream().noneMatch(Thread::isAlive), "Concurrent fill-in failed: " + failed.get());
        }
    }

    private static void update() throws Exception {
        try (Harness h = new Harness()) {
            Intent initial = new Intent(ACTION).setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_GRANT_READ_URI_PERMISSION)
                    .putExtra("old", "value");
            IIntentSender token = h.create(ActivityManager.INTENT_SENDER_ACTIVITY, 5,
                    new Intent[]{new Intent("old-stack"), initial}, new String[]{null, null}, PendingIntent.FLAG_IMMUTABLE);
            Intent replacement = new Intent(ACTION).setFlags(Intent.FLAG_ACTIVITY_CLEAR_TASK | Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
                    .putExtra("new", "value");
            Intent prefix = new Intent("new-stack");
            IIntentSender reused = h.create(ActivityManager.INTENT_SENDER_ACTIVITY, 5,
                    new Intent[]{prefix, replacement}, new String[]{"prefix/type", null},
                    PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
            check(reused == token, "Immutable UPDATE_CURRENT did not reuse token");
            Intent stored = h.am.getIntentForIntentSender(token);
            check(stored.getFlags() == initial.getFlags(), "UPDATE_CURRENT replaced the creator's non-key intent flags");
            check(!stored.hasExtra("old") && "value".equals(stored.getStringExtra("new")), "UPDATE_CURRENT did not replace final extras");
            Intent[] stack = (Intent[])get(token, "intents");
            check("new-stack".equals(stack[0].getAction()), "UPDATE_CURRENT did not refresh preceding stack intents");
            prefix.setAction("mutated"); replacement.putExtra("new", "mutated"); stored.putExtra("new", "readback-mutation");
            check("new-stack".equals(((Intent[])get(token, "intents"))[0].getAction())
                    && "value".equals(h.am.getIntentForIntentSender(token).getStringExtra("new")), "Creator/readback copies share stored mutable state");
        }
    }

    private static void cancellation() throws Exception {
        try (Harness h = new Harness()) {
            Intent base = new Intent(ACTION); IIntentSender first = h.create(6, base, null, MUTABLE);
            Cancelled retained = new Cancelled(), removed = new Cancelled(), dead = new Cancelled(), broken = new Cancelled();
            check(h.am.registerIntentSenderCancelListenerEx(first, retained), "Live token rejected cancel listener");
            check(h.am.registerIntentSenderCancelListenerEx(first, retained), "Duplicate listener rejected live token");
            h.am.registerIntentSenderCancelListenerEx(first, removed); h.am.unregisterIntentSenderCancelListener(first, removed);
            h.am.registerIntentSenderCancelListenerEx(first, dead); dead.token.die();
            broken.action = () -> { throw new IllegalStateException("Expected local cancellation listener failure"); };
            h.am.registerIntentSenderCancelListenerEx(first, broken);
            AtomicReference<Throwable> reentrantFailure = new AtomicReference<>();
            retained.action = () -> { try {
                check(!Thread.holdsLock(get(h.pending, "mRecords")), "Cancellation callback held record lock");
                IIntentSender another = h.create(60, base, null, MUTABLE); h.am.cancelIntentSender(another);
            } catch (Throwable failure) { reentrantFailure.set(failure); } };
            h.am.cancelIntentSender(first); h.am.cancelIntentSender(first); h.flush();
            check(retained.calls.get() == 1 && broken.calls.get() == 1 && removed.calls.get() == 0 && dead.calls.get() == 0,
                    "Cancellation listeners were omitted, duplicated, or ignored unregister/death");
            check(reentrantFailure.get() == null, "Cancellation reentry failed: " + reentrantFailure.get());
            check(!h.am.registerIntentSenderCancelListenerEx(first, new Cancelled()), "Cancelled token accepted listener");
            check(retained.token.recipients.isEmpty(), "Cancellation retained death recipient after delivery");
            IIntentSender old = h.create(7, base, null, MUTABLE); Cancelled oldListener = new Cancelled();
            h.am.registerIntentSenderCancelListenerEx(old, oldListener);
            IIntentSender replacement = h.create(7, base, null, MUTABLE | PendingIntent.FLAG_CANCEL_CURRENT);
            h.flush(); check(oldListener.calls.get() == 1, "CANCEL_CURRENT omitted cancellation");
            Cancelled fresh = new Cancelled(); h.am.registerIntentSenderCancelListenerEx(replacement, fresh);
            oldListener.token.die(); h.am.cancelIntentSender(old); h.flush();
            check(fresh.calls.get() == 0, "Old token death/cancel retired its replacement");
            IIntentSender once = h.create(8, base, null, MUTABLE | PendingIntent.FLAG_ONE_SHOT); Cancelled consumed = new Cancelled();
            h.am.registerIntentSenderCancelListenerEx(once, consumed); h.send(once, 0, null, null, new Receiver()); h.flush();
            check(consumed.calls.get() == 1, "One-shot consumption omitted cancellation");
            IIntentSender stale = h.create(9, base, null, MUTABLE); Cancelled invalid = new Cancelled();
            h.am.registerIntentSenderCancelListenerEx(stale, invalid); h.replaceCreator(9);
            check(h.send(stale, 0, null, null, null) == ActivityManager.START_CANCELED, "Changed signer retained token authority");
            h.flush(); check(invalid.calls.get() == 1, "Invalid creator generation omitted cancellation");
            IIntentSender removal = h.create(10, base, null, MUTABLE); Cancelled removedPackage = new Cancelled();
            h.am.registerIntentSenderCancelListenerEx(removal, removedPackage);
            invoke(h.pm, "revokePackageCapabilities", new Class<?>[]{String.class}, CREATOR); h.flush();
            check(removedPackage.calls.get() == 1 && h.send(removal, 0, null, null, null) == ActivityManager.START_CANCELED,
                    "Package removal did not cancel/notify its tokens");
        }
    }

    private static void completion() throws Exception {
        try (Harness h = new Harness()) {
            IIntentSender token = h.create(11, new Intent(ACTION), null, MUTABLE);
            Receiver empty = new Receiver();
            check(h.send(token, 71, null, null, empty) == 0 && empty.calls.get() == 1 && empty.code == 71,
                    "Empty broadcast discarded the PendingIntent completion callback or initial code");
            Receiver direct = new Receiver(); token.send(72, null, null, null, direct, null, null);
            check(direct.calls.get() == 1 && direct.code == 72, "Direct IIntentSender.send discarded completion");
            Receiver broken = new Receiver(); broken.action = () -> { throw new IllegalStateException("Expected finished callback failure"); };
            check(h.send(token, 0, null, null, broken) == 0, "Throwing finished receiver escaped send");
            Receiver after = new Receiver(); h.send(token, 1, null, null, after);
            check(after.calls.get() == 1, "Finished receiver failure blocked subsequent sends");
            for (int kind : new int[]{ActivityManager.INTENT_SENDER_SERVICE, ActivityManager.INTENT_SENDER_FOREGROUND_SERVICE,
                    ActivityManager.INTENT_SENDER_ACTIVITY}) {
                IIntentSender operation = h.create(kind, 12 + kind, new Intent[]{new Intent("gesher.pending.UNHANDLED")}, null, MUTABLE);
                Receiver finished = new Receiver(); int result = h.send(operation, 91, null, null, finished);
                check(result != ActivityManager.START_CANCELED && finished.calls.get() == 1 && finished.code == 0,
                        "Nonbroadcast dispatch omitted completion or reused the caller's result code: " + kind);
            }
            h.am.cancelIntentSender(token); Receiver cancelled = new Receiver();
            check(h.send(token, 0, null, null, cancelled) == ActivityManager.START_CANCELED && cancelled.calls.get() == 0,
                    "Cancelled send delivered completion");
        }
        try (Harness h = new Harness()) {
            Receiver target = h.receiver(0, null), finished = new Receiver();
            IIntentSender token = h.create(19, new Intent(ACTION), null, MUTABLE);
            check(h.am.sendIntentSender(null, token, null, 81, null, null, finished,
                    "gesher.pending.UNGRANTED", null) == 0 && target.calls.get() == 0
                    && finished.calls.get() == 1 && finished.code == 81,
                    "Acknowledged PendingIntent bypassed its required recipient permission");
            IIntentSender protectedToken = h.create(19, new Intent(ACTION + ".PROTECTED"), null, MUTABLE);
            Receiver denied = new Receiver();
            h.asHolder(() -> {
                rejects(SecurityException.class, () -> h.send(protectedToken, 0, null, null, denied));
                check(Binder.getCallingUid() == 12352 && Binder.getCallingPid() == h.holderPid,
                        "Rejected acknowledged broadcast did not restore holder identity");
            });
            check(denied.calls.get() == 0, "Rejected protected broadcast delivered completion");
        }
        try (Harness h = new Harness()) {
            Receiver hidden = h.receiver(true, Context.RECEIVER_NOT_EXPORTED, null, 0, null);
            Receiver guarded = h.receiver(true, Context.RECEIVER_EXPORTED, "gesher.pending.UNGRANTED", 0, null);
            Receiver finished = new Receiver();
            IIntentSender token = h.create(19, new Intent(ACTION), null, MUTABLE);
            h.asHolder(() -> check(h.send(token, 82, null, null, finished) == 0,
                    "Holder could not send a creator capability to an empty permitted receiver set"));
            check(hidden.calls.get() == 0 && guarded.calls.get() == 0
                    && finished.calls.get() == 1 && finished.code == 82,
                    "Acknowledged PendingIntent used holder/system authority for receiver export or permission checks");
        }
        try (Harness h = new Harness()) {
            List<String> order = new ArrayList<>();
            Receiver low = h.receiver(10, () -> { order.add("low"); h.finish(303, "last", bundle(3)); });
            Receiver high = h.receiver(100, () -> order.add("high")); Receiver finished = new Receiver();
            finished.action = () -> order.add("final");
            IIntentSender once = h.create(20, new Intent(ACTION), null, MUTABLE | PendingIntent.FLAG_ONE_SHOT);
            h.asHolder(() -> check(h.send(once, 101, null, null, finished) == 0, "Holder send rejected verified creator capability"));
            check(order.equals(List.of("high")) && high.code == 101 && high.ordered && finished.calls.get() == 0,
                    "PendingIntent completion ran before async receiver finish");
            check(CREATOR.equals(h.ownerThread.sendingPackage) && h.ownerThread.sendingUid == 12351,
                    "Capability send was attributed to the holder/system instead of creator");
            BroadcastReceiver.PendingResult pending = new BroadcastReceiver.PendingResult(high.code, high.data, high.extras,
                    BroadcastReceiver.PendingResult.TYPE_REGISTERED, true, false, false, h.ownerThread.binder, 0, 0, 12351, CREATOR);
            pending.setResult(202, "middle", bundle(2)); h.asOwner(() -> pending.sendFinished(h.am));
            check(order.equals(List.of("high", "low", "final")) && low.code == 202 && "middle".equals(low.data)
                    && low.extras.getInt("value") == 2 && finished.calls.get() == 1 && finished.code == 303
                    && "last".equals(finished.data) && finished.extras.getInt("value") == 3 && !finished.ordered,
                    "One-shot broadcast lost receiver order, acknowledged results or final callback");
            h.finish(9, null, null); check(finished.calls.get() == 1, "Duplicate finish replayed completion");
            check(h.send(once, 0, null, null, new Receiver()) == ActivityManager.START_CANCELED, "One-shot sent twice");
        }
        try (Harness h = new Harness()) {
            h.receiver(0, null); Receiver finished = new Receiver();
            IIntentSender token = h.create(21, new Intent(ACTION), null, MUTABLE);
            h.send(token, 42, null, null, finished);
            invoke(h.atm, "processDied", new Class<?>[]{h.processType}, h.ownerProcess);
            check(finished.calls.get() == 1 && finished.code == 42, "Receiver death lost PendingIntent completion");
        }
        try (Harness h = new Harness(100)) {
            h.receiver(0, null); Receiver finished = new Receiver();
            h.send(h.create(22, new Intent(ACTION), null, MUTABLE), 43, null, null, finished);
            check(finished.received.await(5, TimeUnit.SECONDS) && finished.calls.get() == 1 && finished.code == 43,
                    "Receiver timeout lost PendingIntent completion");
        }
        try (Harness h = new Harness()) {
            Receiver never = h.receiver(0, null); Receiver finished = new Receiver();
            IIntentSender token = h.create(23, new Intent(ACTION), null, MUTABLE);
            set(h.pending, "mBeforeSendDispatch", (Runnable)() -> { try { h.replaceCreator(1); }
                catch (Exception failure) { throw new AssertionError(failure); } });
            check(h.send(token, 0, null, null, finished) == ActivityManager.START_CANCELED
                    && never.calls.get() == 0 && finished.calls.get() == 0, "Retired generation reached dispatch/completion");
        }
        try (Harness h = new Harness()) {
            h.receiver(0, null); Receiver finished = new Receiver();
            h.send(h.create(24, new Intent(ACTION), null, MUTABLE | PendingIntent.FLAG_ONE_SHOT), 1, null, null, finished);
            h.replaceCreator(1); h.finish(2, null, null);
            check(finished.calls.get() == 0, "Accepted broadcast delivered final callback across creator generation replacement");
        }
    }

    private static IActivityManager wire(IActivityManager implementation) throws Exception {
        Class<?> standIn = type("StandIn"); standIn.getMethod("setStrict", boolean.class).invoke(null, true);
        Object stub = standIn.getMethod("of", Class.class, IInterface.class).invoke(null, IActivityManager.class, implementation);
        IBinder target = (IBinder)standIn.getMethod("asBinder").invoke(stub);
        return IActivityManager.Stub.asInterface(new Binder() {
            @Override protected boolean onTransact(int code, Parcel request, Parcel reply, int flags) throws RemoteException {
                return target.transact(code, request, reply, flags);
            }
        });
    }
    private static void publicApi(boolean realBinder) throws Exception {
        Object singleton = field(ActivityManager.class, "IActivityManagerSingleton").get(null);
        Field instance = field(android.util.Singleton.class, "mInstance"); Object previous = instance.get(singleton);
        Field current = field(ActivityThread.class, "sCurrentActivityThread"); Object previousThread = current.get(null);
        try (Harness h = new Harness()) {
            ActivityThread thread = (ActivityThread)UNSAFE.allocateInstance(ActivityThread.class);
            Class<?> appThreadType = Class.forName("android.app.ActivityThread$ApplicationThread");
            set(thread, "mAppThread", make(appThreadType, new Class<?>[]{ActivityThread.class}, thread)); current.set(null, thread);
            instance.set(singleton, realBinder ? wire(h.am) : h.am);
            PendingIntent pi = new PendingIntent(h.create(30, new Intent(ACTION), null, MUTABLE));
            AtomicInteger completed = new AtomicInteger(); AtomicReference<Intent> delivered = new AtomicReference<>();
            pi.send(55, (sent, intent, code, data, extras) -> {
                check(sent == pi && code == 55 && ACTION.equals(intent.getAction()), "Public OnFinished lost its arguments");
                completed.incrementAndGet(); delivered.set(intent);
            }, null);
            check(completed.get() == 1 && delivered.get() != null, "Public PendingIntent.send never invoked OnFinished");
            AtomicInteger cancellations = new AtomicInteger();
            check(pi.addCancelListener(Runnable::run, ignored -> cancellations.incrementAndGet()), "Public cancel listener registration failed");
            pi.cancel(); h.flush();
            check(cancellations.get() == 1, "Public PendingIntent cancel listener was never notified");
            rejects(PendingIntent.CanceledException.class, () -> pi.send(0, (a,b,c,d,e) -> completed.incrementAndGet(), null));
            check(completed.get() == 1, "Cancelled public send called OnFinished");
        } finally { current.set(null, previousThread); instance.set(singleton, previous); }
    }

    public static void contracts(String selected, boolean realBinder) throws Exception {
        if (selected.equals("all") || selected.equals("identity")) identity();
        if (selected.equals("all") || selected.equals("fill")) fillIn();
        if (selected.equals("all") || selected.equals("update")) update();
        if (selected.equals("all") || selected.equals("cancel")) cancellation();
        if (selected.equals("all") || selected.equals("completion")) completion();
        if (selected.equals("all") || selected.equals("public")) publicApi(realBinder);
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        contracts(args.length == 0 ? "all" : args[0], true);
        System.out.println("PendingIntentContractTest all passed: " + checks + " key, fill-in, update, callback and lifetime checks");
    }
}
