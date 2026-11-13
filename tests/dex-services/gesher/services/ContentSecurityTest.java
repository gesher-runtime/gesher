/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.accounts.Account;
import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.content.ContentResolver;
import android.content.ComponentName;
import android.content.IContentService;
import android.content.Intent;
import android.content.ISyncContext;
import android.content.SyncResult;
import android.content.res.AssetManager;
import android.database.IContentObserver;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.LinkedHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BiPredicate;
import java.util.function.Supplier;

/** Actual content/URI-grant/sync policy with local fixture metadata; no APK code or network. */
public final class ContentSecurityTest {
    private static final String OWNER = "com.gesher.devsvc", CLIENT = "com.gesher.microgprobe";
    private static final Uri URI = Uri.parse("content://com.gesher.devsvc.notes/one");
    private static final int READ = Intent.FLAG_GRANT_READ_URI_PERMISSION, WRITE = Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
    private static void require(boolean value, String message) { if (!value) throw new AssertionError(message); }
    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name); f.setAccessible(true); return f;
    }
    private static Method method(Class<?> type, String name, Class<?>... parameters) throws Exception {
        Method m = type.getDeclaredMethod(name, parameters); m.setAccessible(true); return m;
    }
    private static Object construct(Class<?> type, Class<?>[] parameters, Object... values) throws Exception {
        Constructor<?> c = type.getDeclaredConstructor(parameters); c.setAccessible(true); return c.newInstance(values);
    }
    private interface Operation { void run() throws Exception; }
    private static void denied(Operation operation) throws Exception {
        try { operation.run(); } catch (SecurityException expected) { return; }
        throw new AssertionError("Unauthorized content/sync operation was accepted");
    }
    private static final class Observer extends IContentObserver.Stub {
        final AtomicInteger changes = new AtomicInteger();
        Operation beforeDelivery;
        @Override public void onChange(boolean self, Uri uri, int user) { changes.incrementAndGet(); }
        @Override public void onChangeEtc(boolean self, Uri[] uris, int flags, int user) {
            if (beforeDelivery != null) {
                try { beforeDelivery.run(); } catch (Exception e) { throw new AssertionError(e); }
            }
            changes.addAndGet(uris.length);
        }
    }
    private static final class AppThread extends IApplicationThread.Default {
        final Binder binder = new Binder();
        final AtomicInteger bindings;
        AppThread(AtomicInteger bindings) { this.bindings = bindings; }
        @Override public IBinder asBinder() { return binder; }
        @Override public void scheduleBindService(IBinder token, IBinder bindToken, Intent intent,
                boolean rebind, int processState, long sequence) { bindings.incrementAndGet(); }
    }
    private static final class Harness {
        final Class<?> apkType = Class.forName("com.gesher.server.ApkPackage");
        final Class<?> pmType = Class.forName("com.gesher.server.PackageManagerService");
        final Class<?> atmType = Class.forName("com.gesher.server.ActivityTaskManagerService");
        final Class<?> amType = Class.forName("com.gesher.server.ActivityManagerService");
        final Class<?> syncType = Class.forName("com.gesher.server.SyncManager");
        final Object owner, client, pm, atm, sync;
        final IActivityManager am;
        final IContentService content;
        final int ownerUid, clientUid;
        final Account hidden = new Account("hidden@example.test", "com.gesher.devsvc.account");
        final Account visible = new Account("visible@example.test", "com.gesher.devsvc.account");
        volatile CountDownLatch visibilityChecked = new CountDownLatch(1);
        volatile boolean showClient;
        final AtomicInteger bindings = new AtomicInteger();
        int completedBindings;
        IApplicationThread ownerThread;
        @SuppressWarnings("unchecked")
        Harness(String ownerPath, String clientPath) throws Exception {
            owner = apkType.getMethod("parse", String.class).invoke(null, ownerPath);
            client = apkType.getMethod("parse", String.class).invoke(null, clientPath);
            ownerUid = (Integer) apkType.getMethod("uid").invoke(owner);
            clientUid = (Integer) apkType.getMethod("uid").invoke(client);
            pm = construct(pmType, new Class<?>[]{Collection.class}, Arrays.asList(owner, client));
            atm = construct(atmType, new Class<?>[]{pmType}, pm);
            Class<?> notifications = Class.forName("com.gesher.server.NotificationManagerService");
            am = (IActivityManager) construct(amType, new Class<?>[]{atmType, pmType, notifications},
                    atm, pm, construct(notifications, new Class<?>[0]));
            sync = construct(syncType, new Class<?>[]{amType, pmType}, am, pm);
            content = (IContentService) construct(Class.forName("com.gesher.server.ContentService"), new Class<?>[]{syncType}, sync);
            Class<?> process = Class.forName(atmType.getName() + "$ProcessRecord");
            for (String pkg : new String[]{OWNER, CLIENT}) {
                int pid = pkg.equals(OWNER) ? 812301 : 812302;
                AppThread thread = new AppThread(bindings);
                if (pkg.equals(OWNER)) ownerThread = thread;
                as(pkg, () -> {
                    Object record = construct(process, new Class<?>[]{atmType, int.class, IApplicationThread.class}, atm, pid, thread);
                    field(process, "packageName").set(record, pkg);
                    field(process, "processName").set(record, pkg);
                    field(process, "ready").setBoolean(record, true);
                    ((List<Object>) field(atmType, "mProcesses").get(atm)).add(record);
                });
            }
            // Keep the fake owner process alive by actual started-service policy while
            // successive sync bindings come and go; no fixture APK classes execute.
            Object services = field(amType, "mServices").get(am);
            method(services.getClass(), "startService", IApplicationThread.class, Intent.class,
                    String.class, boolean.class, String.class).invoke(services, null,
                    new Intent().setComponent(new ComponentName(OWNER, OWNER + ".PrivateAccountService")),
                    null, false, "android");
            field(syncType, "mAccounts").set(sync, (Supplier<Account[]>) () -> new Account[]{hidden, visible});
            field(syncType, "mAccountAccess").set(sync, (BiPredicate<Account, String>) (account, pkg) -> {
                if (OWNER.equals(pkg)) return true;
                if (CLIENT.equals(pkg)) {
                    visibilityChecked.countDown();
                    return showClient && account.equals(visible);
                }
                return false;
            });
            require(((List<?>) method(syncType, "adapters").invoke(sync)).size() == 1,
                    "Fresh devsvc fixture must declare its real local Notes SyncAdapter XML");
        }
        void as(String pkg, Operation operation) throws Exception {
            long original = Binder.clearCallingIdentity();
            try {
                int uid = pkg.equals(OWNER) ? ownerUid : clientUid;
                int pid = pkg.equals(OWNER) ? 812301 : pkg.equals(CLIENT) ? 812302 : 812399;
                Binder.restoreCallingIdentity(((long) uid << 32) | pid);
                require(Binder.getCallingPid() == pid && Binder.getCallingUid() == uid, "Native Binder identity unavailable");
                operation.run();
            } finally { Binder.restoreCallingIdentity(original); }
        }
        void notify(String pkg, Uri uri, int flags) throws Exception {
            as(pkg, () -> content.notifyChange(new Uri[]{uri}, null, false, flags, 0, 36, pkg));
        }
        Map<?, ?> map(String name) throws Exception {
            synchronized (sync) { return new LinkedHashMap<>((Map<?, ?>) field(syncType, name).get(sync)); }
        }
        void grant(int modes) throws Exception { am.grantUriPermission(null, CLIENT, URI, modes, 0); }
        void revoke(int modes) throws Exception {
            am.revokeUriPermission(Binder.getCallingPid() == 812301 ? ownerThread : null, CLIENT, URI, modes, 0);
        }
        void flushUpload() throws Exception {
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(7);
            while (!map("mUploads").isEmpty() && System.nanoTime() < deadline) Thread.sleep(25);
            require(map("mUploads").isEmpty(), "Delayed upload callback did not run");
            ((ScheduledExecutorService) field(syncType, "mTimer").get(sync)).submit(() -> {}).get(3, TimeUnit.SECONDS);
        }
        void noSync() throws Exception {
            synchronized (sync) {
                require(((List<?>) field(syncType, "mPending").get(sync)).isEmpty()
                        && map("mActive").isEmpty(),
                        "Unauthorized request queued or started a hidden account sync");
            }
        }
        void finishVisibleSync() throws Exception {
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
            while ((map("mActive").isEmpty() || bindings.get() <= completedBindings)
                    && System.nanoTime() < deadline) Thread.sleep(25);
            require(map("mActive").size() == 1, "Ordinary visible-account client failed to start exactly one sync");
            require(bindings.get() > completedBindings, "Visible sync did not actually bind its adapter");
            completedBindings = bindings.get();
            Object active = map("mActive").values().iterator().next();
            require(visible.equals(field(active.getClass(), "account").get(active)), "Hidden account reached the adapter");
            ((ISyncContext) active).onFinished(new SyncResult());
            ((ScheduledExecutorService) field(syncType, "mTimer").get(sync)).submit(() -> {}).get(3, TimeUnit.SECONDS);
            noSync();
        }
    }

    public static void main(String[] args) throws Exception {
        require(args.length == 2, "Pass fresh devsvc.apk and microgprobe.apk fixture paths");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false, System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        android.system.Os.setenv("ANDROID_DATA", Files.createTempDirectory("gesher-content-security-").toString(), true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true);
        Harness h = new Harness(args[0], args[1]);
        Observer ownerObserver = new Observer(), clientObserver = new Observer();
        h.as(OWNER, () -> h.content.registerContentObserver(URI, true, ownerObserver, 0, 36));
        h.as(CLIENT, () -> {
            denied(() -> h.content.registerContentObserver(URI, true, clientObserver, 0, 36));
            denied(() -> h.content.notifyChange(new Uri[]{URI}, null, false, ContentResolver.NOTIFY_SYNC_TO_NETWORK, 0, 36, OWNER));
            denied(() -> h.content.notifyChange(new Uri[]{URI}, null, false, ContentResolver.NOTIFY_SYNC_TO_NETWORK, 0, 36, CLIENT));
            denied(() -> h.content.setMasterSyncAutomatically(false));
            denied(() -> h.content.setSyncAutomatically(h.hidden, URI.getAuthority(), false));
            denied(() -> h.content.setIsSyncable(h.hidden, URI.getAuthority(), 0));
            denied(() -> h.content.addPeriodicSync(h.hidden, URI.getAuthority(), new Bundle(), 900));
            denied(() -> h.content.setMasterSyncAutomaticallyAsUser(false, 10));
            denied(() -> h.content.registerContentObserver(URI, true, clientObserver, 10, 36));
        });
        h.as("unknown", () -> {
            denied(() -> h.content.registerContentObserver(Uri.parse("content://settings/system"), true, clientObserver, 0, 36));
            denied(() -> h.content.setMasterSyncAutomatically(false));
        });
        h.as(OWNER, () -> {
            h.content.setMasterSyncAutomatically(false);
            require(!h.content.getMasterSyncAutomatically(), "Declared sync-settings writer was denied");
            h.content.setMasterSyncAutomatically(true);
        });
        h.noSync();
        h.notify(OWNER, URI, 0);
        require(ownerObserver.changes.get() == 1 && clientObserver.changes.get() == 0, "Provider owner notification failed or leaked");
        h.grant(READ);
        h.as(CLIENT, () -> h.content.registerContentObserver(URI, true, clientObserver, 0, 36));
        h.notify(OWNER, URI, 0);
        require(clientObserver.changes.get() == 1, "Explicit read grant did not authorize observation");
        h.notify(OWNER, Uri.withAppendedPath(URI, "private-child"), 0);
        require(clientObserver.changes.get() == 1, "Exact read grant leaked descendant URI changes");
        ownerObserver.beforeDelivery = () -> h.revoke(READ);
        try { h.notify(OWNER, URI, 0); } finally { ownerObserver.beforeDelivery = null; }
        require(clientObserver.changes.get() == 1, "Earlier observer callback revoked access but later delivery still leaked a URI");
        h.grant(READ);
        h.grant(WRITE);
        h.notify(CLIENT, URI, 0);
        require(clientObserver.changes.get() == 2, "Explicit write grant did not authorize notification");
        h.revoke(READ);
        h.notify(OWNER, URI, 0);
        require(clientObserver.changes.get() == 2, "Revoked read grant still received content URIs");
        System.out.println("ContentSecurityTest: private observations/notifiers, package spoof, unknown PID, sync-settings and cross-user writes denied; owner and exact URI grants work/revoke");

        h.notify(CLIENT, URI, ContentResolver.NOTIFY_SYNC_TO_NETWORK);
        require(h.visibilityChecked.await(6, TimeUnit.SECONDS), "Delayed sync lost the originating app account-visibility check");
        h.flushUpload();
        h.noSync();
        h.showClient = true;
        h.as(CLIENT, () -> h.content.requestSync(null, URI.getAuthority(), new Bundle(), CLIENT));
        h.finishVisibleSync();
        h.notify(CLIENT, URI, ContentResolver.NOTIFY_SYNC_TO_NETWORK);
        h.flushUpload();
        h.finishVisibleSync();
        System.out.println("ContentSecurityTest: real delayed upload filters hidden accounts; ordinary direct and upload clients sync only visible accounts");

        h.notify(CLIENT, URI, ContentResolver.NOTIFY_SYNC_TO_NETWORK);
        h.revoke(WRITE);
        h.flushUpload();
        h.noSync();
        h.grant(WRITE);
        h.notify(CLIENT, URI, ContentResolver.NOTIFY_SYNC_TO_NETWORK);
        Object replacement = h.apkType.getMethod("parse", String.class).invoke(null, args[1]);
        method(h.pmType, "add", h.apkType).invoke(h.pm, replacement);
        h.flushUpload();
        h.noSync();
        h.notify(OWNER, URI, 0);
        require(clientObserver.changes.get() == 2, "Replaced observer identity received URI data");
        System.out.println("ContentSecurityTest: queued upload rechecks revoked write grant and replaced notifier code; stale observer identity receives nothing");
        System.out.println("ContentSecurityTest all passed");
    }
}
