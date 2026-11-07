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
        Object ownerProcess = construct(processType,
                new Class<?>[] {atmType, int.class, IApplicationThread.class}, atm, 31701, thread());
        Object senderProcess = construct(processType,
                new Class<?>[] {atmType, int.class, IApplicationThread.class}, atm, 31702, thread());
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
        Object otherOwner = construct(processType,
                new Class<?>[] {atmType, int.class, IApplicationThread.class}, atm, 31703, thread());
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
