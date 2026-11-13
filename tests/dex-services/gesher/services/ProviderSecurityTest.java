/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.app.ContentProviderHolder;
import android.app.IApplicationThread;
import android.content.IContentProvider;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.res.AssetManager;
import android.net.Uri;
import android.os.Binder;
import android.os.IBinder;

import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.Arrays;
import java.util.Collection;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Actual provider/URI-grant policy regressions, without provider code or network. */
public final class ProviderSecurityTest {
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
    }
    private interface Operation { void run() throws Exception; }
    private static void denied(Operation action) throws Exception {
        try { action.run(); }
        catch (SecurityException expected) { return; }
        catch (InvocationTargetException wrapped) {
            if (wrapped.getCause() instanceof SecurityException) return;
            throw wrapped;
        }
        throw new AssertionError("Unauthorized provider operation succeeded");
    }
    private static Method method(Class<?> type, String name, Class<?>... params) throws Exception {
        Method result = type.getDeclaredMethod(name, params); result.setAccessible(true); return result;
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field result = type.getDeclaredField(name); result.setAccessible(true); return result;
    }
    private static Object construct(Class<?> type, Class<?>[] params, Object... args) throws Exception {
        Constructor<?> constructor = type.getDeclaredConstructor(params);
        constructor.setAccessible(true); return constructor.newInstance(args);
    }
    private static void as(int pid, int uid, Operation operation) throws Exception {
        long original = Binder.clearCallingIdentity();
        try {
            Binder.restoreCallingIdentity(((long) uid << 32) | (pid & 0xffffffffL));
            require(Binder.getCallingPid() == pid && Binder.getCallingUid() == uid,
                    "Native Binder calling identity injection is unavailable");
            operation.run();
        } finally { Binder.restoreCallingIdentity(original); }
    }
    private static Object process(Class<?> processType, Class<?> atmType, Object atm,
            int pid, int uid) throws Exception {
        Object[] result = new Object[1];
        as(pid, uid, () -> result[0] = construct(processType,
                new Class<?>[] {atmType, int.class, IApplicationThread.class}, atm, pid, thread()));
        return result[0];
    }

    /** A reference caller blocked on ATM must not already hold the provider monitor. */
    private static void providerLockOrder(Object atm, Object providers, Object ownerProcess,
            Field lastActive, Method ref, Method retire, IBinder connection, int pid, int uid)
            throws Exception {
        AtomicReference<Throwable> failure = new AtomicReference<>();
        CountDownLatch held = new CountDownLatch(1);
        Thread reference = new Thread(() -> {
            try {
                as(pid, uid, () -> require((Boolean) ref.invoke(providers, connection, 0, 0),
                        "A live provider reference reported dead"));
            } catch (Throwable problem) { failure.compareAndSet(null, problem); }
        }, "provider-reference-lock-order");
        Thread retiring = new Thread(() -> {
            try {
                synchronized (atm) {
                    held.countDown();
                    long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
                    while (reference.getState() != Thread.State.BLOCKED && failure.get() == null
                            && System.nanoTime() < deadline) Thread.sleep(1);
                    require(reference.getState() == Thread.State.BLOCKED,
                            "Reference caller did not reach ATM authentication");
                    require(!(Boolean) retire.invoke(providers, ownerProcess,
                                    lastActive.getLong(ownerProcess)),
                            "Retirement ignored a held provider reference");
                }
            } catch (Throwable problem) { failure.compareAndSet(null, problem); }
        }, "provider-retirement-lock-order");
        reference.setDaemon(true);
        retiring.setDaemon(true);
        retiring.start();
        require(held.await(3, TimeUnit.SECONDS), "Retirement did not acquire ATM");
        reference.start();
        retiring.join(3_000);
        reference.join(3_000);
        require(!retiring.isAlive() && !reference.isAlive(),
                "Provider reference authentication inverted the ATM/provider lock order");
        if (failure.get() != null) throw new AssertionError("Provider lock-order regression", failure.get());
    }

    /** A lookup authenticated before death cannot recreate the dead client's connections. */
    @SuppressWarnings("unchecked")
    private static void clientDeathDuringLookup(Class<?> atmType, Class<?> processType, Object atm,
            Object providers, Method counted, String callerPackage, String authority, int uid)
            throws Exception {
        Object dying = process(processType, atmType, atm, 31704, uid);
        field(processType, "packageName").set(dying, callerPackage);
        field(processType, "processName").set(dying, callerPackage);
        synchronized (atm) {
            ((List<Object>) field(atmType, "mProcesses").get(atm)).add(dying);
        }
        Method processDied = method(atmType, "processDied", processType);
        Field dead = field(processType, "dead");
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread lookup = new Thread(() -> {
            try {
                as(31704, uid, () -> denied(() -> counted.invoke(providers,
                        dying, callerPackage, authority, false, true)));
            } catch (Throwable problem) { failure.compareAndSet(null, problem); }
        }, "provider-lookup-during-client-death");
        Thread death = new Thread(() -> {
            try { processDied.invoke(atm, dying); }
            catch (Throwable problem) { failure.compareAndSet(null, problem); }
        }, "provider-client-death");
        lookup.setDaemon(true);
        death.setDaemon(true);
        synchronized (providers) {
            lookup.start();
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
            while (lookup.getState() != Thread.State.BLOCKED && failure.get() == null
                    && System.nanoTime() < deadline) Thread.sleep(1);
            require(lookup.getState() == Thread.State.BLOCKED,
                    "Lookup did not reach provider admission after authentication");
            death.start();
            deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
            while (!dead.getBoolean(dying) && failure.get() == null
                    && System.nanoTime() < deadline) Thread.sleep(1);
            require(dead.getBoolean(dying), "Physical death did not retire the process generation");
        }
        lookup.join(3_000);
        death.join(3_000);
        require(!lookup.isAlive() && !death.isAlive(), "Provider lookup/death regression did not finish");
        if (failure.get() != null) throw new AssertionError("Provider lookup/death regression", failure.get());
        require(!field(processType, "exiting").getBoolean(dying),
                "Physical death was confused with intentional exit and would suppress service recovery");
        for (Object connection : ((java.util.Map<?, ?>)
                field(providers.getClass(), "mConnections").get(providers)).values()) {
            require(field(connection.getClass(), "client").get(connection) != dying,
                    "A delayed lookup recreated the dead client's connection");
        }
    }

    private static IApplicationThread thread() {
        return new IApplicationThread.Default() {
            private final Binder binder = new Binder();
            @Override public IBinder asBinder() { return binder; }
        };
    }
    private static ContentProviderHolder holder(ProviderInfo info) {
        ContentProviderHolder holder = new ContentProviderHolder(new ProviderInfo(info));
        holder.info.applicationInfo = new ApplicationInfo(info.applicationInfo);
        Binder binder = new Binder("android.content.IContentProvider");
        holder.provider = (IContentProvider) Proxy.newProxyInstance(
                IContentProvider.class.getClassLoader(), new Class<?>[] {IContentProvider.class},
                (p, m, a) -> m.getName().equals("asBinder") ? binder : null);
        return holder;
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        Class<?> apk = Class.forName("com.gesher.server.ApkPackage");
        Object sender = apk.getMethod("parse", String.class).invoke(null,
                new File(args[0], "v2only.apk").getPath());
        Object owner = apk.getMethod("parse", String.class).invoke(null, args[1]);
        String senderName = (String) apk.getField("packageName").get(sender);
        String ownerName = (String) apk.getField("packageName").get(owner);
        int senderUid = (Integer) apk.getMethod("uid").invoke(sender);
        int ownerUid = (Integer) apk.getMethod("uid").invoke(owner);
        List<ProviderInfo> declarations = (List<ProviderInfo>) apk.getMethod("providers", long.class)
                .invoke(owner, (long) PackageManager.GET_META_DATA);
        ProviderInfo privateProvider = null;
        for (ProviderInfo info : declarations) {
            if (!info.exported && info.grantUriPermissions) { privateProvider = info; break; }
        }
        require(privateProvider != null, "Official GmsCore lacks a private URI-grant provider fixture");
        final ProviderInfo provider = privateProvider;
        final String authority = provider.authority.split(";")[0];
        Class<?> pmType = Class.forName("com.gesher.server.PackageManagerService");
        Class<?> atmType = Class.forName("com.gesher.server.ActivityTaskManagerService");
        Class<?> amType = Class.forName("com.gesher.server.ActivityManagerService");
        Class<?> notifications = Class.forName("com.gesher.server.NotificationManagerService");
        Object pm = construct(pmType, new Class<?>[] {Collection.class}, Arrays.asList(sender, owner));
        Object atm = construct(atmType, new Class<?>[] {pmType}, pm);
        Object am = construct(amType, new Class<?>[] {atmType, pmType, notifications}, atm, pm,
                construct(notifications, new Class<?>[0]));
        Object providers = field(amType, "mProviders").get(am);
        Class<?> cpType = providers.getClass();
        Class<?> processType = Class.forName(atmType.getName() + "$ProcessRecord");
        Object ownerProcess = process(processType, atmType, atm, 31701, ownerUid);
        Object senderProcess = process(processType, atmType, atm, 31702, senderUid);
        field(processType, "packageName").set(ownerProcess, ownerName);
        field(processType, "processName").set(ownerProcess, provider.processName);
        field(processType, "packageName").set(senderProcess, senderName);
        field(processType, "processName").set(senderProcess, senderName);
        ((List<Object>) field(atmType, "mProcesses").get(atm)).addAll(Arrays.asList(ownerProcess, senderProcess));
        Method get = method(cpType, "getContentProvider", processType, String.class, String.class);
        Method publish = method(cpType, "publishContentProviders", processType, List.class);

        ContentProviderHolder local = (ContentProviderHolder) get.invoke(providers, ownerProcess, ownerName, authority);
        require(local.mLocal && local.provider == null, "same-package unpublished provider deadlocks instead of installing locally");
        denied(() -> get.invoke(providers, senderProcess, senderName, authority));
        ProviderInfo guarded = new ProviderInfo(provider);
        guarded.exported = true;
        guarded.readPermission = "android.permission.WRITE_SECURE_SETTINGS";
        guarded.writePermission = "android.permission.WRITE_SECURE_SETTINGS";
        Method access = method(cpType, "enforceAccess", String.class, ProviderInfo.class);
        denied(() -> access.invoke(providers, senderName, guarded));
        ContentProviderHolder wrongAuthority = holder(provider); wrongAuthority.info.authority += ";test.hijack";
        denied(() -> publish.invoke(providers, ownerProcess, List.of(wrongAuthority)));
        ContentProviderHolder wrongUid = holder(provider); wrongUid.info.applicationInfo.uid++;
        denied(() -> publish.invoke(providers, ownerProcess, List.of(wrongUid)));
        ContentProviderHolder wrongComponent = holder(provider); wrongComponent.info.name = "test.ForgedProvider";
        denied(() -> publish.invoke(providers, ownerProcess, List.of(wrongComponent)));
        denied(() -> publish.invoke(providers, senderProcess, List.of(holder(provider))));
        require(((java.util.Map<?, ?>) field(cpType, "mPublished").get(providers)).isEmpty(),
                "a rejected publication partially changed the authority map");

        ContentProviderHolder mutable = holder(provider); mutable.info.exported = true;
        mutable.info.readPermission = null; mutable.info.writePermission = null;
        publish.invoke(providers, ownerProcess, List.of(mutable));
        denied(() -> get.invoke(providers, senderProcess, senderName, authority));
        Object otherOwner = process(processType, atmType, atm, 31703, ownerUid);
        field(processType, "packageName").set(otherOwner, ownerName);
        field(processType, "processName").set(otherOwner, provider.processName);
        ((List<Object>) field(atmType, "mProcesses").get(atm)).add(otherOwner);
        denied(() -> publish.invoke(providers, otherOwner, List.of(holder(provider))));
        System.out.println("ProviderSecurityTest: local sibling, canonical metadata and publication/authority/UID barriers passed");

        Object grants = method(cpType, "uriGrants").invoke(providers); Class<?> grantsType = grants.getClass();
        Method grant = method(grantsType, "grant", String.class, Uri.class, int.class, int.class);
        Method check = method(grantsType, "check", Uri.class, int.class, int.class, int.class, int.class);
        Method revoke = method(grantsType, "revoke", String.class, Uri.class, int.class, int.class);
        Uri uri = Uri.parse("content://" + authority + "/shared/item");
        int read = Intent.FLAG_GRANT_READ_URI_PERMISSION, write = Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
        require((Integer) check.invoke(grants, uri, 31702, senderUid, read, 0) == PackageManager.PERMISSION_DENIED,
                "unknown URI grant defaulted to granted");
        require((Integer) amType.getMethod("checkUriPermission", Uri.class, int.class, int.class,
                int.class, int.class, IBinder.class).invoke(am, uri, 31702, senderUid, read, 0, null)
                        == PackageManager.PERMISSION_DENIED,
                "AM URI permission inherited the old permissive default");
        require((Integer) check.invoke(grants, uri, 31702, 1000, read, 0) == PackageManager.PERMISSION_DENIED,
                "forged system UID authorized a non-system PID");
        grant.invoke(grants, senderName, uri, read, 0);
        require((Integer) check.invoke(grants, uri, 31702, senderUid, read, 0) == PackageManager.PERMISSION_GRANTED,
                "explicit owner/host read grant missing");
        require((Integer) check.invoke(grants, uri, 31702, senderUid, write, 0) == PackageManager.PERMISSION_DENIED,
                "read grant became write access");
        require((Integer) check.invoke(grants, uri.buildUpon().appendPath("child").build(), 31702,
                senderUid, read, 0) == PackageManager.PERMISSION_DENIED, "exact grant became prefix access");
        require((Integer) check.invoke(grants, uri, 31702, senderUid, read, 10) == PackageManager.PERMISSION_DENIED,
                "URI grant crossed user boundary");
        ContentProviderHolder acquired = (ContentProviderHolder) get.invoke(providers, senderProcess, senderName, authority);
        require(acquired != null && acquired.provider != null && !acquired.info.exported,
                "URI-granted holder lost trusted private-provider metadata");
        require(acquired.noReleaseNeeded && acquired.connection == null,
                "a server-local acquisition asked for a release");
        // ActivityThread's reference counting (ContentProviderConnection).
        Method counted = method(cpType, "getContentProvider", processType, String.class,
                String.class, boolean.class, boolean.class);
        Method inUse = method(cpType, "hasActiveClients", processType);
        Method ref = method(cpType, "refContentProvider", IBinder.class, int.class, int.class);
        Method remove = method(cpType, "removeContentProvider", IBinder.class, boolean.class);
        ContentProviderHolder unstable = (ContentProviderHolder) counted.invoke(providers,
                senderProcess, senderName, authority, false, true);
        require(unstable.connection != null && !unstable.noReleaseNeeded,
                "a client's holder carries no connection to release");
        require((Boolean) inUse.invoke(providers, ownerProcess), "an unstable reference left the provider unused");
        Method retire = method(cpType, "retireUnused", processType, long.class);
        Field lastActive = field(processType, "lastActive");
        Field exiting = field(processType, "exiting");
        Field reaped = field(processType, "reaped");
        as(31702, senderUid, () -> require((Boolean) ref.invoke(providers, unstable.connection, 0, 0),
                "The authenticated client could not retain its own provider"));
        as(31701, ownerUid, () -> denied(() -> ref.invoke(providers, unstable.connection, 0, 1)));
        as(31702, senderUid + 1, () -> denied(() -> remove.invoke(providers, unstable.connection, false)));
        exiting.setBoolean(senderProcess, true);
        try {
            as(31702, senderUid, () -> denied(() -> ref.invoke(providers, unstable.connection, 0, 0)));
        } finally { exiting.setBoolean(senderProcess, false); }
        require(!(Boolean) retire.invoke(providers, ownerProcess, lastActive.getLong(ownerProcess))
                        && !exiting.getBoolean(ownerProcess) && !reaped.getBoolean(ownerProcess),
                "A held reference caused a temporary or committed provider exit");
        require(((java.util.Map<?, ?>) field(cpType, "mPublished").get(providers)).containsKey(authority),
                "A rejected retirement discarded the live publisher");
        providerLockOrder(atm, providers, ownerProcess, lastActive, ref, retire,
                unstable.connection, 31702, senderUid);
        clientDeathDuringLookup(atmType, processType, atm, providers, counted,
                senderName, authority, senderUid);
        ContentProviderHolder stable = (ContentProviderHolder) counted.invoke(providers,
                senderProcess, senderName, authority, true, true);
        require(stable.connection == unstable.connection, "one client got two connections to one provider");
        // installProvider lost a race: a second stable holder, released at once.
        ContentProviderHolder duplicate = (ContentProviderHolder) counted.invoke(providers,
                senderProcess, senderName, authority, true, true);
        require(duplicate.connection == stable.connection, "a duplicate holder got its own connection");
        remove.invoke(providers, duplicate.connection, true);
        require((Boolean) inUse.invoke(providers, ownerProcess), "a held stable reference was dropped");
        // The unstable reference released while a stable one is held: refContentProvider(0, -1).
        require((Boolean) ref.invoke(providers, stable.connection, 0, -1), "a live connection reported dead");
        require((Boolean) inUse.invoke(providers, ownerProcess), "a held stable reference was dropped");
        // releaseProvider of the last stable reference: stable to one unstable, then removal.
        require((Boolean) ref.invoke(providers, stable.connection, -1, 1), "a live connection reported dead");
        require((Boolean) inUse.invoke(providers, ownerProcess), "a pending removal left the provider unused");
        remove.invoke(providers, stable.connection, false);
        require(!(Boolean) inUse.invoke(providers, ownerProcess), "a released provider stayed in use");
        require(!(Boolean) ref.invoke(providers, stable.connection, 0, 1), "a removed connection was revived");
        ContentProviderHolder again = (ContentProviderHolder) counted.invoke(providers,
                senderProcess, senderName, authority, false, true);
        require(again.connection != null && again.connection != stable.connection
                && (Boolean) inUse.invoke(providers, ownerProcess), "a new acquisition after removal was not counted");
        method(cpType, "processDied", processType).invoke(providers, senderProcess);
        require(!(Boolean) inUse.invoke(providers, ownerProcess), "a dead client kept its references");
        long previousUse = lastActive.getLong(ownerProcess);
        method(processType, "markActive", long.class).invoke(ownerProcess, previousUse + 1);
        require(!(Boolean) retire.invoke(providers, ownerProcess, previousUse)
                        && !exiting.getBoolean(ownerProcess) && !reaped.getBoolean(ownerProcess),
                "Retirement ignored a newer use or exposed a temporary exit");
        require(((java.util.Map<?, ?>) field(cpType, "mPublished").get(providers)).containsKey(authority),
                "A stale retirement discarded the live publisher");
        require((Boolean) retire.invoke(providers, ownerProcess, lastActive.getLong(ownerProcess))
                        && exiting.getBoolean(ownerProcess) && reaped.getBoolean(ownerProcess)
                        && !((java.util.Map<?, ?>) field(cpType, "mPublished").get(providers)).containsKey(authority),
                "A committed retirement did not atomically retire the unused publisher");
        denied(() -> publish.invoke(providers, ownerProcess, List.of(holder(provider))));
        // Restore the fixture for the URI revocation checks below.
        exiting.setBoolean(ownerProcess, false);
        reaped.setBoolean(ownerProcess, false);
        publish.invoke(providers, ownerProcess, List.of(holder(provider)));
        System.out.println("ProviderSecurityTest: provider counts, connection ownership, lock order and atomic retirement passed");
        revoke.invoke(grants, senderName, uri, read, 0);
        require((Integer) check.invoke(grants, uri, 31702, senderUid, read, 0) == PackageManager.PERMISSION_DENIED,
                "URI revoke did not remove read access");
        denied(() -> get.invoke(providers, senderProcess, senderName, authority));
        grant.invoke(grants, senderName, uri, read | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION, 0);
        require((Integer) check.invoke(grants, uri.buildUpon().appendPath("child").build(), 31702,
                senderUid, read, 0) == PackageManager.PERMISSION_GRANTED, "explicit prefix grant missing");
        require((Integer) check.invoke(grants, Uri.parse("content://" + authority + "/shared/items"),
                31702, senderUid, read, 0) == PackageManager.PERMISSION_DENIED,
                "URI prefix did not honor a path-segment boundary");
        revoke.invoke(grants, senderName, uri, read, 0);
        Class<?> managerType = Class.forName("com.gesher.server.UriGrantsManagerService");
        Object manager = construct(managerType, new Class<?>[] {pmType, atmType, amType}, pm, atm, am);
        Method take = method(managerType, "takePersistableUriPermission", Uri.class, int.class,
                String.class, int.class);
        try {
            take.invoke(manager, uri, read, senderName, 0);
            throw new AssertionError("Persistable URI support was invented");
        } catch (InvocationTargetException unsupported) {
            require(unsupported.getCause() instanceof UnsupportedOperationException,
                    "Persistable URI API failed without an explicit unsupported outcome");
        }
        System.out.println("ProviderSecurityTest: explicit ephemeral URI read grant/revoke, path, UID and user boundaries passed");
        System.out.println("ProviderSecurityTest all passed");
    }
}
