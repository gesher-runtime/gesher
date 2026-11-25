/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.IApplicationThread;
import android.content.IContentProvider;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.net.Uri;
import android.os.Binder;
import android.os.IBinder;
import com.android.internal.pm.pkg.component.ParsedProvider;
import com.gesher.server.ActivityManagerService;
import com.gesher.server.ApkPackage;
import com.gesher.server.PackageManagerService;
import com.gesher.server.UriGrants;
import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.AbstractList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import sun.misc.Unsafe;

/** Actual service retirement after the existing fixture's admitted metadata publication. */
public final class UriGrantRetirementContractTest {
    static final String OWNER = "fixture.uri.owner", CLIENT = "fixture.uri.client";
    static final String OTHER = "fixture.uri.other", PEER = "fixture.uri.shared.peer";
    static final int READ = Intent.FLAG_GRANT_READ_URI_PERMISSION;
    static final int WRITE = Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
    static final int PREFIX = Intent.FLAG_GRANT_PREFIX_URI_PERMISSION;
    static final int PERSISTABLE = Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION;
    private static int checks;

    interface Action { void run() throws Exception; }

    static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }

    static Field field(Class<?> type, String name) throws Exception {
        Field result = type.getDeclaredField(name);
        result.setAccessible(true);
        return result;
    }

    static Object get(Object target, String name) throws Exception {
        return field(target.getClass(), name).get(target);
    }

    static Object call(Object target, String name, Class<?>[] types, Object... args) throws Exception {
        Method method = target.getClass().getDeclaredMethod(name, types);
        method.setAccessible(true);
        try { return method.invoke(target, args); }
        catch (InvocationTargetException wrapped) {
            Throwable cause = wrapped.getCause();
            if (cause instanceof Exception e) throw e;
            if (cause instanceof Error e) throw e;
            throw wrapped;
        }
    }

    static Uri uri(int number) { return Uri.parse("content://fixture.uri.provider/retirement/" + number); }

    /** Reuse metadata construction, not a duplicate permission implementation. */
    static final class Fixture implements AutoCloseable {
        final Object harness;
        final PackageManagerService pm;
        final ActivityManagerService am;
        final UriGrants grants;
        final Object providers;
        final Map<String, ApkPackage> records;
        final Map<String, Integer> pids;
        final Map<String, IApplicationThread> threads;

        @SuppressWarnings("unchecked")
        Fixture() throws Exception {
            Class<?> type = Class.forName("gesher.services.PersistableUriGrantContractTest$Harness");
            Constructor<?> constructor = type.getDeclaredConstructor(File.class, boolean.class);
            constructor.setAccessible(true);
            harness = constructor.newInstance(null, true);
            pm = (PackageManagerService) get(harness, "pm");
            am = (ActivityManagerService) get(harness, "amService");
            grants = (UriGrants) get(harness, "grants");
            providers = get(harness, "providers");
            records = (Map<String, ApkPackage>) get(harness, "records");
            pids = (Map<String, Integer>) get(harness, "pids");
            threads = (Map<String, IApplicationThread>) get(harness, "threads");
        }

        void as(String name, Action operation) throws Exception {
            long previous = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long) records.get(name).uid() << 32) | pids.get(name));
                operation.run();
            } finally { Binder.restoreCallingIdentity(previous); }
        }

        void grant(String target, Uri uri, int modes) {
            am.grantUriPermission(threads.get(OWNER), target, uri, modes, 0);
        }

        boolean explicit(String target, Uri uri, int modes) {
            return am.checkUriPermission(uri, pids.get(target), records.get(target).uid(), modes, 0, null)
                    == PackageManager.PERMISSION_GRANTED;
        }

        void seed(String target, int count) throws Exception {
            as(OWNER, () -> { for (int i = 0; i < count; i++) grant(target, uri(i), READ); });
        }

        void replace(String name) throws Exception {
            call(harness, "replace", new Class<?>[]{String.class}, name);
        }

        IBinder owner() throws Exception {
            return (IBinder) call(grants, "newOwner", new Class<?>[0]);
        }

        void owned(IBinder owner, String target, Intent intent) throws Exception {
            Object prepared = call(grants, "prepareIntent", new Class<?>[]{int.class, String.class, Intent.class},
                    records.get(OWNER).uid(), target, intent);
            call(grants, "publish", new Class<?>[]{IBinder.class, List.class}, owner, prepared);
        }

        void retire(IBinder owner) throws Exception {
            call(grants, "revokeByOwner", new Class<?>[]{IBinder.class}, owner);
        }

        int grantCount(String target) throws Exception {
            Map<?, ?> table = (Map<?, ?>) ((Map<?, ?>) get(grants, "mGrants")).get(records.get(target).uid());
            return table == null ? 0 : table.size();
        }

        Object hosted(String authority) throws Exception {
            ProviderInfo info = new ProviderInfo();
            info.name = "fixture.hosted.Provider";
            info.packageName = "fixture.hosted";
            info.authority = authority;
            info.exported = true;
            info.grantUriPermissions = true;
            info.readPermission = "fixture.hosted.READ";
            info.writePermission = "fixture.hosted.WRITE";
            info.applicationInfo = new ApplicationInfo();
            info.applicationInfo.packageName = info.packageName;
            info.applicationInfo.uid = android.os.Process.SYSTEM_UID;
            // Registration and grants are production code; this test does not read provider data.
            IContentProvider transport = (IContentProvider) Proxy.newProxyInstance(
                    IContentProvider.class.getClassLoader(), new Class<?>[]{IContentProvider.class},
                    (self, method, args) -> { throw new AssertionError("Unexpected provider call: " + method); });
            Class<?> type = Class.forName("com.gesher.server.ContentProviders$RegisteredProvider");
            Constructor<?> constructor = type.getDeclaredConstructor(ProviderInfo.class, IContentProvider.class);
            constructor.setAccessible(true);
            Object registration = constructor.newInstance(info, transport);
            call(providers, "registerProvider", new Class<?>[]{type}, registration);
            return registration;
        }

        void hostedGrant(Object registration, String target, Uri uri) throws Exception {
            call(grants, "grantFromProvider", new Class<?>[]{registration.getClass(), String.class, Uri.class, int.class},
                    registration, target, uri, READ);
        }

        @Override public void close() throws Exception {
            call(grants, "clear", new Class<?>[]{String.class}, (Object) null);
            grants.flush();
            Object writer = get(get(pm, "mPermissions"), "mWriter");
            check((Boolean) call(writer, "flush", new Class<?>[0]), "Fixture left a failed permission write");
            check(((Map<?, ?>) get(grants, "mGrants")).isEmpty(), "Fixture left a grant table");
            check(((Map<?, ?>) get(grants, "mOwned")).isEmpty(), "Fixture left a strong owner membership");
        }
    }

    public static void initialize(boolean loadNative) throws Exception {
        if (loadNative) System.loadLibrary("android_runtime");
        Field singleton = Unsafe.class.getDeclaredField("theUnsafe");
        singleton.setAccessible(true);
        field(PersistableUriGrantContractTest.class, "unsafe").set(null, singleton.get(null));
        field(PersistableUriGrantContractTest.class, "binder").setBoolean(null, loadNative);
    }

    private static void repeatedAndReplaced() throws Exception {
        for (int count : new int[]{1, 8, 128, 512}) {
            try (Fixture f = new Fixture()) {
                f.seed(CLIENT, count);
                f.seed(OTHER, count);
                for (int repeat = 0; repeat < 3; repeat++) {
                    for (int i = 0; i < count; i++) {
                        Uri value = uri(i);
                        f.as(CLIENT, () -> {
                            check(f.explicit(CLIENT, value, READ), "A current exact grant disappeared");
                            check(!f.explicit(CLIENT, value, WRITE), "Retirement state cached a mode verdict");
                            check(f.explicit(PEER, value, READ), "Shared UID lost the canonical grant");
                        });
                    }
                }
                f.replace(OWNER);
                f.as(CLIENT, () -> check(!f.explicit(CLIENT, uri(0), READ), "Replaced provider retained a transient grant"));
                f.as(OTHER, () -> check(!f.explicit(OTHER, uri(0), READ), "One UID sweep validated another UID"));
                check(f.grantCount(CLIENT) == 0 && f.grantCount(OTHER) == 0,
                        "Provider replacement left stale tables or owner references");
                f.seed(CLIENT, count);
                f.as(CLIENT, () -> check(f.explicit(CLIENT, uri(0), READ), "Fresh grant failed after replacement"));
                f.replace(CLIENT);
                f.as(CLIENT, () -> check(!f.explicit(CLIENT, uri(0), READ), "Replacement target inherited old transient modes"));
                f.as(PEER, () -> check(!f.explicit(PEER, uri(0), READ), "Shared UID peer revived a retired canonical target"));
                check(f.grantCount(CLIENT) == 0, "Target replacement did not prune its stale table");
            }
        }
    }

    private static void liveModesAndRemoval() throws Exception {
        try (Fixture f = new Fixture()) {
            f.seed(CLIENT, 128);
            f.as(CLIENT, () -> check(f.explicit(CLIENT, uri(0), READ), "Initial warm grant failed"));
            ApkPackage owner = f.records.get(OWNER);
            call(f.pm, "beginPackageRemoval", new Class<?>[]{ApkPackage.class}, owner);
            f.as(CLIENT, () -> check(!f.explicit(CLIENT, uri(0), READ), "Pending source removal was memoized away"));
            call(f.pm, "endPackageRemoval", new Class<?>[]{ApkPackage.class}, owner);
            f.as(CLIENT, () -> check(f.explicit(CLIENT, uri(0), READ), "Aborted source removal erased a live grant"));
            ApkPackage target = f.records.get(CLIENT);
            Object epoch = get(target, "mEnabledOwner");
            field(ApkPackage.class, "mEnabledOwner").set(target, "retired-installation");
            f.as(CLIENT, () -> check(!f.explicit(CLIENT, uri(0), READ), "In-place installation epoch change was missed"));
            field(ApkPackage.class, "mEnabledOwner").set(target, epoch);
            f.as(CLIENT, () -> check(f.explicit(CLIENT, uri(0), READ), "Live installation epoch was not reread"));
            f.as(OWNER, () -> f.am.revokeUriPermission(f.threads.get(OWNER), CLIENT, uri(0), READ, 0));
            f.as(CLIENT, () -> check(!f.explicit(CLIENT, uri(0), READ), "Explicit revocation reused a prior verdict"));
            f.as(CLIENT, () -> check(f.explicit(CLIENT, uri(1), READ), "Revocation crossed into another URI"));
            f.as(CLIENT, () -> check(f.am.checkUriPermission(uri(1), f.pids.get(CLIENT),
                    f.records.get(CLIENT).uid() + 100000, READ, 0, null) == PackageManager.PERMISSION_DENIED,
                    "User-stripped UID inherited another user's grant"));
            call(f.pm, "revokePackageCapabilities", new Class<?>[]{String.class}, CLIENT);
            check(f.grantCount(CLIENT) == 0, "Permanent capability removal did not clear the actual table");
        }
    }

    private static void ownedBatchAndPrefix() throws Exception {
        try (Fixture f = new Fixture()) {
            IBinder owner = f.owner();
            Intent root = new Intent().setData(uri(0)).addFlags(READ | PREFIX);
            android.content.ClipData clip = android.content.ClipData.newRawUri("URI grant batch", uri(1));
            for (int i = 2; i < 256; i++) clip.addItem(new android.content.ClipData.Item(uri(i)));
            clip.addItem(new android.content.ClipData.Item(uri(0)));
            root.setClipData(clip);
            f.as(OWNER, () -> f.owned(owner, CLIENT, root));
            check(f.grantCount(CLIENT) == 256, "Duplicate URI changed canonical membership");
            f.as(CLIENT, () -> {
                for (int i = 0; i < 256; i++) {
                    check(f.explicit(CLIENT, uri(i).buildUpon().appendPath("child").build(), READ),
                            "Prefix grant stopped matching descendants");
                }
            });
            f.retire(owner);
            check(f.grantCount(CLIENT) == 0, "Retiring a real batch owner left permissions");
            f.as(CLIENT, () -> check(!f.explicit(CLIENT, uri(0), READ), "Retired owner was cached as authorized"));
        }
    }

    private static void publicationBeforeInsertion() throws Exception {
        try (Fixture f = new Fixture()) {
            f.seed(CLIENT, 128);
            f.as(CLIENT, () -> check(f.explicit(CLIENT, uri(0), READ), "Initial retirement state was not exercised"));
            f.replace(OWNER);
            f.as(OWNER, () -> f.grant(CLIENT, uri(1000), READ));
            check(f.grantCount(CLIENT) == 1, "Fresh insertion retained obsolete source grants");
            f.as(CLIENT, () -> check(f.explicit(CLIENT, uri(1000), READ), "New source generation failed to grant"));
            f.replace(CLIENT);
            f.as(OWNER, () -> f.grant(CLIENT, uri(1001), READ));
            check(f.grantCount(CLIENT) == 1, "Fresh insertion retained obsolete target grants");
            f.as(CLIENT, () -> check(f.explicit(CLIENT, uri(1001), READ), "New target generation failed to receive a grant"));
        }
    }

    private static void clearPublicationIdentity(PackageManagerService pm) throws Exception {
        // Unsafe-created prepublication fixtures have no token. The unchanged
        // baseline has no such field at all and already performs every sweep.
        try {
            Field generation = field(PackageManagerService.class, "mPackageGeneration");
            synchronized (pm) { generation.set(pm, null); }
        } catch (NoSuchFieldException baselineWithoutIdentity) { }
    }

    private static void unpublishedFixtureStillRetires() throws Exception {
        try (Fixture f = new Fixture()) {
            clearPublicationIdentity(f.pm);
            f.seed(CLIENT, 128);
            f.as(CLIENT, () -> check(f.explicit(CLIENT, uri(0), READ), "Prepublication fixture lost a current grant"));
            f.replace(OWNER);
            clearPublicationIdentity(f.pm);
            f.as(CLIENT, () -> check(!f.explicit(CLIENT, uri(0), READ), "Null publication identity suppressed retirement"));
            check(f.grantCount(CLIENT) == 0, "Prepublication fallback retained old owners");
        }
    }

    private static void removeReaddAndReuse() throws Exception {
        try (Fixture f = new Fixture()) {
            f.seed(CLIENT, 128);
            f.as(CLIENT, () -> check(f.explicit(CLIENT, uri(0), READ), "Initial removal grant missing"));
            Map<String, ApkPackage> missing = new LinkedHashMap<>(f.records);
            missing.remove(OWNER);
            // Exercise actual whole-registry removal/publication without claiming
            // to perform an APK filesystem uninstall or signed UID admission.
            synchronized (f.pm) { call(f.pm, "publishPackages", new Class<?>[]{Map.class}, missing); }
            f.as(CLIENT, () -> check(!f.explicit(CLIENT, uri(0), READ), "Unloaded provider retained a transient grant"));
            check(f.grantCount(CLIENT) == 0, "Unloaded provider retained its UID table");
            synchronized (f.pm) { call(f.pm, "publishPackages", new Class<?>[]{Map.class}, new LinkedHashMap<>(f.records)); }
            f.as(CLIENT, () -> check(!f.explicit(CLIENT, uri(0), READ), "Readding the old metadata object revived a retired grant"));
            f.seed(CLIENT, 1);
            f.as(CLIENT, () -> check(f.explicit(CLIENT, uri(0), READ), "A new UID table could not receive a fresh grant"));
            synchronized (f.pm) { call(f.pm, "publishPackages", new Class<?>[]{Map.class}, new LinkedHashMap<>(f.records)); }
            f.as(CLIENT, () -> check(f.explicit(CLIENT, uri(0), READ), "Unchanged installed objects lost modes during republication"));
        }
    }

    private static void batchRevalidatesLiveModes() throws Exception {
        try (Fixture f = new Fixture()) {
            f.seed(CLIENT, 128);
            Intent intent = new Intent().setData(uri(0)).addFlags(READ);
            android.content.ClipData clip = android.content.ClipData.newRawUri("delegation", uri(1));
            for (int i = 2; i < 128; i++) clip.addItem(new android.content.ClipData.Item(uri(i)));
            intent.setClipData(clip);
            Object[] prepared = new Object[1];
            f.as(CLIENT, () -> prepared[0] = call(f.grants, "prepareIntent",
                    new Class<?>[]{int.class, String.class, Intent.class}, f.records.get(CLIENT).uid(), OTHER, intent));
            f.as(OWNER, () -> f.am.revokeUriPermission(f.threads.get(OWNER), CLIENT, uri(127), READ, 0));
            IBinder owner = f.owner();
            try {
                call(f.grants, "publish", new Class<?>[]{IBinder.class, List.class}, owner, prepared[0]);
                throw new AssertionError("Batch used a permission verdict captured before revocation");
            } catch (SecurityException expected) {
                check(f.grantCount(OTHER) == 0, "Rejected complete batch published a partial grant set");
            }
            f.retire(owner);
        }
    }

    @SuppressWarnings("unchecked")
    private static void hostedFallback() throws Exception {
        try (Fixture f = new Fixture()) {
            f.seed(CLIENT, 128);
            Object registration = f.hosted("fixture.hosted;fixture.hosted.alias");
            ProviderInfo info = (ProviderInfo) get(registration, "info");
            Uri value = Uri.parse("content://fixture.hosted/item");
            f.hostedGrant(registration, CLIENT, value);
            f.as(CLIENT, () -> check(f.explicit(CLIENT, value, READ), "Hosted grant was not admitted"));
            String authority = info.authority;
            synchronized (f.pm) { info.authority = "fixture.missing.hosted"; }
            f.as(CLIENT, () -> check(!f.explicit(CLIENT, value, READ), "Changed hosted authority stayed current"));
            synchronized (f.pm) { info.authority = authority; }
            f.as(CLIENT, () -> check(!f.explicit(CLIENT, value, READ), "Hosted metadata rollback resurrected retired modes"));
            f.as(CLIENT, () -> check(f.explicit(CLIENT, uri(0), READ), "Hosted retirement revoked unrelated APK grant"));

            f.hostedGrant(registration, CLIENT, value);
            Map<String, Object> registry = (Map<String, Object>) get(f.providers, "mRegisteredProviders");
            Map<String, Object> incomplete = new LinkedHashMap<>(registry);
            incomplete.remove("fixture.hosted.alias");
            synchronized (f.pm) { field(f.providers.getClass(), "mRegisteredProviders").set(f.providers, Map.copyOf(incomplete)); }
            f.as(CLIENT, () -> check(!f.explicit(CLIENT, value, READ), "Partial hosted-registration retirement stayed valid"));
            synchronized (f.pm) { field(f.providers.getClass(), "mRegisteredProviders").set(f.providers, registry); }
            f.as(CLIENT, () -> check(!f.explicit(CLIENT, value, READ), "Registry restoration resurrected old hosted transient modes"));
        }
    }

    private static Thread worker(String name, AtomicReference<Throwable> failure, Action action) {
        Thread thread = new Thread(() -> {
            try { action.run(); } catch (Throwable t) { failure.compareAndSet(null, t); }
        }, name);
        thread.setDaemon(true);
        thread.start();
        return thread;
    }

    private static void join(Thread thread, AtomicReference<Throwable> failure) throws Exception {
        thread.join(3000);
        check(!thread.isAlive(), "Owned operation did not retire: " + thread.getName());
        if (failure.get() != null) throw new AssertionError("Owned operation failed", failure.get());
    }

    private static void ownerRetirementWithoutPackageLock() throws Exception {
        try (Fixture f = new Fixture()) {
            IBinder owner = f.owner();
            f.as(OWNER, () -> f.owned(owner, CLIENT, new Intent().setData(uri(0)).addFlags(READ)));
            f.as(CLIENT, () -> check(f.explicit(CLIENT, uri(0), READ), "Initial owned grant missing"));
            AtomicReference<Throwable> failure = new AtomicReference<>();
            CountDownLatch callerStarted = new CountDownLatch(1);
            Thread query;
            synchronized (f.pm) {
                query = worker("uri-query-waiting-for-registry", failure, () -> {
                    callerStarted.countDown();
                    f.as(CLIENT, () -> check(!f.explicit(CLIENT, uri(0), READ),
                            "A waiting query ignored completed owner retirement"));
                });
                check(callerStarted.await(3, TimeUnit.SECONDS), "Query did not start");
                Thread retirement = worker("uri-owner-retirement", failure, () -> f.retire(owner));
                join(retirement, failure);
            }
            join(query, failure);
            check(f.grantCount(CLIENT) == 0, "Owner race left grant membership");
        }
    }

    @SuppressWarnings("unchecked")
    private static void queryAndPublicationUseOneRegistry() throws Exception {
        try (Fixture f = new Fixture()) {
            f.seed(CLIENT, 128);
            ApkPackage owner = f.records.get(OWNER);
            Object parsed = owner.parsed;
            Field providersField = field(parsed.getClass(), "providers");
            List<ParsedProvider> originalProviders = (List<ParsedProvider>) providersField.get(parsed);
            CountDownLatch reading = new CountDownLatch(1), release = new CountDownLatch(1);
            // Keep the actual PackageImpl and ParsedProvider objects. The Android
            // generator also reads concrete PackageImpl state before projecting
            // this list, so a ParsedPackage interface substitute is insufficient.
            List<ParsedProvider> heldMetadata = new AbstractList<>() {
                @Override public int size() { return originalProviders.size(); }
                @Override public ParsedProvider get(int index) {
                    if (Thread.currentThread().getName().equals("uri-registry-query")) {
                        if (!Thread.holdsLock(f.pm)) throw new AssertionError("Provider query lost its registry monitor");
                        reading.countDown();
                        try {
                            if (!release.await(3, TimeUnit.SECONDS)) throw new AssertionError("Metadata fixture was not released");
                        } catch (InterruptedException failure) { throw new AssertionError(failure); }
                    }
                    return originalProviders.get(index);
                }
            };
            providersField.set(parsed, heldMetadata);
            Method create = PersistableUriGrantContractTest.class.getDeclaredMethod("createPackage", String.class, int.class, boolean.class);
            create.setAccessible(true);
            ApkPackage replacement = (ApkPackage) create.invoke(null, OWNER, owner.uid(), true);
            Map<String, ApkPackage> next = new LinkedHashMap<>(f.records);
            next.put(OWNER, replacement);
            AtomicReference<Throwable> failure = new AtomicReference<>();
            AtomicReference<Boolean> result = new AtomicReference<>();
            Thread query = worker("uri-registry-query", failure,
                    () -> result.set(f.grants.canAccess(f.records.get(CLIENT), uri(0), READ, 0)));
            Thread publisher = null;
            CountDownLatch publishStarted = new CountDownLatch(1), published = new CountDownLatch(1);
            try {
                boolean entered = reading.await(3, TimeUnit.SECONDS);
                if (!entered && failure.get() != null) throw new AssertionError("Query failed before provider projection", failure.get());
                check(entered, "Query did not enter real provider projection");
                publisher = worker("uri-registry-publication", failure, () -> {
                    publishStarted.countDown();
                    synchronized (f.pm) { call(f.pm, "publishPackages", new Class<?>[]{Map.class}, next); }
                    published.countDown();
                });
                check(publishStarted.await(3, TimeUnit.SECONDS), "Publication did not start");
                check(published.getCount() == 1, "Publication crossed the held query's package monitor");
            } finally {
                release.countDown();
                join(query, failure);
                if (publisher != null) join(publisher, failure);
                providersField.set(parsed, originalProviders);
            }
            check(Boolean.TRUE.equals(result.get()), "Query mixed old provider metadata with a later registry");
            f.as(CLIENT, () -> check(!f.explicit(CLIENT, uri(0), READ), "Completed publication did not retire old query state"));
            check(f.grantCount(CLIENT) == 0, "Publication/query race left obsolete grant records");
        }
    }

    public static void contracts(boolean loadNative) throws Exception {
        initialize(loadNative);
        checks = 0;
        repeatedAndReplaced();
        liveModesAndRemoval();
        ownedBatchAndPrefix();
        publicationBeforeInsertion();
        unpublishedFixtureStillRetires();
        removeReaddAndReuse();
        batchRevalidatesLiveModes();
        hostedFallback();
        ownerRetirementWithoutPackageLock();
        queryAndPublicationUseOneRegistry();
        System.out.println("UriGrantRetirementContractTest: " + checks + " checks passed");
    }

    public static void main(String[] args) throws Exception { contracts(true); }
}
