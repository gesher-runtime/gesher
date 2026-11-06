/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.app.ContentProviderHolder;
import android.app.IApplicationThread;
import android.content.ComponentName;
import android.content.IContentProvider;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.pm.ServiceInfo;
import android.content.res.AssetManager;
import android.content.res.CompatibilityInfo;
import android.os.Binder;
import android.os.IBinder;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.function.Consumer;

/** Actual component routing and provider isolation; the supplied APK's code is never run. */
public final class ComponentProcessTest {
    private static int checks;
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name); f.setAccessible(true); return f;
    }
    private static Method method(Class<?> type, String name, Class<?>... params) throws Exception {
        Method m = type.getDeclaredMethod(name, params); m.setAccessible(true); return m;
    }
    private static Object construct(Class<?> type, Class<?>[] params, Object... args) throws Exception {
        Constructor<?> c = type.getDeclaredConstructor(params); c.setAccessible(true); return c.newInstance(args);
    }
    private static final class AppThread extends IApplicationThread.Default {
        final Binder binder = new Binder();
        final List<String> created = new ArrayList<>();
        @Override public IBinder asBinder() { return binder; }
        @Override public void scheduleCreateService(IBinder token, ServiceInfo info,
                CompatibilityInfo compat, int state) { created.add(info.name); }
    }

    public static void main(String[] args) throws Exception {
        require(args.length == 2, "Pass APK and a service declared in a secondary process");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        Class<?> apkType = Class.forName("com.gesher.server.ApkPackage");
        Object apk = apkType.getMethod("parse", String.class).invoke(null, args[0]);
        String pkg = (String) apkType.getField("packageName").get(apk);
        Class<?> pmType = Class.forName("com.gesher.server.PackageManagerService");
        Class<?> atmType = Class.forName("com.gesher.server.ActivityTaskManagerService");
        Class<?> amType = Class.forName("com.gesher.server.ActivityManagerService");
        Class<?> notifications = Class.forName("com.gesher.server.NotificationManagerService");
        Class<?> processType = Class.forName(atmType.getName() + "$ProcessRecord");
        Object pm = construct(pmType, new Class<?>[] {Collection.class}, List.of(apk));
        Object atm = construct(atmType, new Class<?>[] {pmType}, pm);
        Object am = construct(amType, new Class<?>[] {atmType, pmType, notifications}, atm, pm,
                construct(notifications, new Class<?>[0]));
        ServiceInfo service = (ServiceInfo) pmType.getMethod("getServiceInfo", ComponentName.class,
                long.class, int.class).invoke(pm, new ComponentName(pkg, args[1]), 0L, 0);
        require(service != null && service.processName != null && !pkg.equals(service.processName),
                "The service fixture must declare its own process");
        Method normalize = method(atmType, "componentProcess", String.class, String.class);
        String remoteName = (String) normalize.invoke(atm, pkg, service.processName);
        require(remoteName.startsWith(pkg + ":"), "Expected a private manifest process");
        AppThread mainThread = new AppThread(), remoteThread = new AppThread();
        Object main = construct(processType, new Class<?>[] {atmType, int.class, IApplicationThread.class},
                atm, 77301, mainThread);
        Object remote = construct(processType, new Class<?>[] {atmType, int.class, IApplicationThread.class},
                atm, 77302, remoteThread);
        for (Object p : List.of(main, remote)) {
            field(processType, "packageName").set(p, pkg);
            field(processType, "ready").setBoolean(p, true);
        }
        field(processType, "processName").set(main, pkg);
        field(processType, "processName").set(remote, remoteName);
        ((List<Object>) field(atmType, "mProcesses").get(atm)).addAll(List.of(main, remote));
        Object[] selected = new Object[1];
        Consumer<Object> capture = p -> selected[0] = p;
        method(atmType, "withProcess", String.class, String.class, Consumer.class)
                .invoke(atm, pkg, "main fixture", capture);
        require(selected[0] == main, "Default work went to the sibling process");
        method(atmType, "withProcess", String.class, String.class, String.class, Consumer.class)
                .invoke(atm, pkg, remoteName, "secondary fixture", capture);
        require(selected[0] == remote, "Declared secondary work went to the main process");
        Object services = field(amType, "mServices").get(am);
        method(services.getClass(), "startService", IApplicationThread.class, Intent.class,
                String.class, boolean.class, String.class).invoke(services, null,
                new Intent().setComponent(new ComponentName(pkg, args[1])), null, false, pkg);
        require(mainThread.created.isEmpty() && remoteThread.created.equals(List.of(args[1])),
                "ActiveServices created the declared secondary service in the wrong application");
        Method authenticated = method(atmType, "authenticatedPackageForPid", int.class);
        require(pkg.equals(authenticated.invoke(atm, 77301)) && pkg.equals(authenticated.invoke(atm, 77302)),
                "Secondary process changed the caller's package identity");
        require(authenticated.invoke(atm, 77303) == null, "Unknown PID gained a package identity");
        Object providers = field(amType, "mProviders").get(am);
        ApplicationInfo ai = (ApplicationInfo) pmType.getMethod("getApplicationInfo", String.class,
                long.class, int.class).invoke(pm, pkg, 0L, 0);
        ApplicationInfo secondaryAi = new ApplicationInfo(ai); secondaryAi.processName = remoteName;
        Method providersFor = method(providers.getClass(), "providersFor", ApplicationInfo.class);
        List<ProviderInfo> mainProviders = (List<ProviderInfo>) providersFor.invoke(providers, ai);
        List<ProviderInfo> remoteProviders = (List<ProviderInfo>) providersFor.invoke(providers, secondaryAi);
        require(!remoteProviders.isEmpty(), "Secondary provider fixture missing");
        for (ProviderInfo p : remoteProviders) {
            require(remoteName.equals(normalize.invoke(atm, pkg, p.processName)), "Wrong secondary provider process");
            require(mainProviders.stream().noneMatch(q -> q.name.equals(p.name)), "Secondary provider installed in main process");
        }
        ProviderInfo declared = remoteProviders.get(0);
        ContentProviderHolder holder = new ContentProviderHolder(new ProviderInfo(declared));
        Binder providerBinder = new Binder("android.content.IContentProvider");
        holder.provider = (IContentProvider) Proxy.newProxyInstance(IContentProvider.class.getClassLoader(),
                new Class<?>[] {IContentProvider.class}, (p, m, a) -> m.getName().equals("asBinder") ? providerBinder : null);
        Method publish = method(providers.getClass(), "publishContentProviders", processType, List.class);
        boolean denied = false;
        try { publish.invoke(providers, main, List.of(holder)); }
        catch (InvocationTargetException e) {
            if (e.getCause() instanceof SecurityException) denied = true;
            else throw e;
        }
        require(denied, "Main process could publish its secondary process's provider");
        publish.invoke(providers, remote, List.of(holder));
        require(!((java.util.Map<?, ?>) field(providers.getClass(), "mPublished").get(providers)).isEmpty(),
                "Correct process could not publish its own provider");
        System.out.println("ComponentProcessTest all passed: " + checks);
    }
}
