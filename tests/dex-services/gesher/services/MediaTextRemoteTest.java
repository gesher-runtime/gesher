/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.ActivityThread;
import android.app.IApplicationThread;
import android.content.Context;
import android.content.pm.IPackageManager;
import android.media.IMediaRouterService;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.service.textservice.SpellCheckerService;
import android.os.UserHandle;
import android.view.textservice.SentenceSuggestionsInfo;
import android.view.textservice.SpellCheckerSession;
import android.view.textservice.SuggestionsInfo;
import android.view.textservice.TextServicesManager;

import com.android.internal.os.ApplicationSharedMemory;
import com.android.internal.textservice.ITextServicesManager;
import com.gesher.app.Main;
import com.gesher.server.ApkPackage;
import com.gesher.server.Services;
import com.gesher.server.SystemServer;

import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Collections;
import java.util.List;
import java.util.Map;

/**
 * Separate Windows ART processes, generated service stubs, ordinary client UID, remote
 * callbacks and real Binder-death cleanup. Only process enrollment is fixture state:
 * a private test control binds the client's real PID/application-thread to the one parsed
 * fixture APK. Production CallerIdentity, service registry and dispatch are unchanged.
 */
public final class MediaTextRemoteTest {
    private static final String CONTROL = "gesher.media.text.contract";
    private static int checks;
    private static IBinder deathProbe;
    private static void check(boolean condition, String reason) {
        if (!condition) throw new AssertionError(reason);
        checks++;
    }
    private static Field field(Class<?> type, String name) throws NoSuchFieldException {
        Field result = type.getDeclaredField(name); result.setAccessible(true); return result;
    }
    private static void bootstrap(String name) throws Exception {
        String root = System.getenv("ANDROID_ROOT");
        Main.bootstrap(name, root + "/build.prop", root + "/framework/framework-res.apk");
        ApplicationSharedMemory.setInstance(ApplicationSharedMemory.create());
    }

    @SuppressWarnings("unchecked")
    private static void server(String ready, String apkPath) throws Exception {
        bootstrap("media-text-server");
        check(Process.myUid() == Process.SYSTEM_UID, "system server UID");
        ApkPackage apk = ApkPackage.parse(apkPath);
        boolean declaresSpellChecker = false;
        for (var service : apk.parsed.getServices()) {
            for (var intent : service.getIntents()) {
                declaresSpellChecker |= intent.getIntentFilter().hasAction(SpellCheckerService.SERVICE_INTERFACE);
            }
        }
        check(!declaresSpellChecker, "no-provider transport fixture must not declare a spell checker");
        Map<String, IBinder> services = Services.create(Collections.singletonList(apk));
        final int clientUid = apk.uid();
        check(UserHandle.isApp(clientUid), "fixture APK must have an admitted ordinary application UID");
        check(IPackageManager.Stub.asInterface(services.get("package"))
                        .getApplicationInfo(apk.packageName, 0, 0).uid == clientUid,
                "admitted package and PackageManager UID agree");
        Object created = field(Services.class, "sLast").get(null);
        Object atm = field(created.getClass(), "atm").get(created);
        List<Object> processes = (List<Object>) field(atm.getClass(), "mProcesses").get(atm);
        IBinder mediaBinder = services.get(Context.MEDIA_ROUTER_SERVICE);
        Object mediaProxy = field(mediaBinder.getClass(), "mImpl").get(mediaBinder);
        Object handler = Proxy.getInvocationHandler(mediaProxy);
        Object media = field(handler.getClass(), "mImpl").get(handler);
        Object mediaLock = field(media.getClass(), "mLock").get(media);
        Map<?, ?> routers = (Map<?, ?>) field(media.getClass(), "mRouters").get(media);
        Map<?, ?> clients = (Map<?, ?>) field(media.getClass(), "mClients").get(media);
        for (Map.Entry<String, IBinder> entry : services.entrySet()) {
            ServiceManager.addService(entry.getKey(), entry.getValue());
        }
        ServiceManager.addService(CONTROL, new Binder() {
            @Override protected boolean onTransact(int code, Parcel request, Parcel reply, int flags)
                    throws RemoteException {
                request.enforceInterface(CONTROL);
                if (code == 1) {
                    if (Binder.getCallingUid() != clientUid) throw new SecurityException("fixture UID");
                    IBinder threadBinder = request.readStrongBinder();
                    if (threadBinder == null) throw new IllegalArgumentException("application thread");
                    int pid = Binder.getCallingPid();
                    try {
                        Class<?> recordType = Class.forName(atm.getClass().getName() + "$ProcessRecord");
                        Constructor<?> constructor = recordType.getDeclaredConstructor(atm.getClass(),
                                int.class, IApplicationThread.class);
                        constructor.setAccessible(true);
                        Object record = constructor.newInstance(atm, pid,
                                IApplicationThread.Stub.asInterface(threadBinder));
                        field(recordType, "packageName").set(record, apk.packageName);
                        field(recordType, "processName").set(record, apk.packageName);
                        field(recordType, "packageCode").set(record, apk);
                        field(recordType, "ready").setBoolean(record, true);
                        synchronized (atm) {
                            check(processes.isEmpty(), "only one fixture process is enrolled");
                            processes.add(record);
                        }
                        threadBinder.linkToDeath(() -> {
                            synchronized (atm) { processes.remove(record); }
                        }, 0);
                    } catch (ReflectiveOperationException failure) {
                        throw new IllegalStateException("fixture enrollment failed", failure);
                    }
                    reply.writeNoException();
                    reply.writeInt(Process.myUid());
                    reply.writeInt(Binder.getCallingUid());
                    return true;
                }
                if (code == 2) {
                    if (Binder.getCallingUid() != Process.SYSTEM_UID) throw new SecurityException("fixture verifier UID");
                    reply.writeNoException();
                    synchronized (mediaLock) { reply.writeInt(routers.size()); reply.writeInt(clients.size()); }
                    synchronized (atm) { reply.writeInt(processes.size()); }
                    return true;
                }
                return super.onTransact(code, request, reply, flags);
            }
        });
        SystemServer.startBinderThreads("media-text-server", 4);
        java.nio.file.Path readyPath = new File(ready).toPath();
        java.nio.file.Path staging = new File(ready + ".writing").toPath();
        Files.write(staging, (apk.packageName + "\n" + clientUid + "\n").getBytes(StandardCharsets.UTF_8));
        Files.move(staging, readyPath);
        System.out.println("MediaTextRemoteTest server ready"); System.out.flush();
        Binder.joinThreadPool();
    }

    private interface Operation { void run() throws Exception; }
    private static void denied(Operation action, String reason) throws Exception {
        try { action.run(); }
        catch (SecurityException expected) { checks++; return; }
        throw new AssertionError(reason);
    }

    private static void client(String packageName, int expectedUid) throws Exception {
        bootstrap("media-text-client");
        check(UserHandle.isApp(expectedUid) && Process.myUid() == expectedUid
                        && Binder.getCallingUid() == expectedUid, "ordinary client native/Binder UID");
        IPackageManager packages = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
        check(packages.getApplicationInfo(packageName, 0, 0).uid == expectedUid,
                "client and remote PackageManager canonical UID agree");
        IMediaRouterService media = IMediaRouterService.Stub.asInterface(
                ServiceManager.getService(Context.MEDIA_ROUTER_SERVICE));
        check(media != null && !(media.asBinder() instanceof Binder), "media service is an actual BinderProxy");
        denied(() -> media.getSystemRoutes(packageName, false), "unenrolled process was accepted");
        Context context = MediaRouterContractTest.context(packageName);
        SystemServer.startBinderThreads("media-text-client", 3);
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            request.writeInterfaceToken(CONTROL);
            IApplicationThread applicationThread = ActivityThread.currentActivityThread().getApplicationThread();
            request.writeStrongBinder(applicationThread.asBinder());
            check(ServiceManager.getService(CONTROL).transact(1, request, reply, 0), "private enrollment transaction");
            reply.readException(); check(reply.readInt() == Process.SYSTEM_UID, "enrollment server identity");
            check(reply.readInt() == expectedUid, "server observed the real ordinary client Binder UID");
        } finally { reply.recycle(); request.recycle(); }
        denied(() -> media.getSystemRoutes("another.package", false), "cross-package media query was accepted");
        denied(() -> media.registerManager(null, packageName), "ordinary app acquired manager authority");
        denied(() -> media.getSystemSessionInfoForPackage(packageName, "another.package"),
                "ordinary app acquired another package's session");

        ITextServicesManager text = ITextServicesManager.Stub.asInterface(
                ServiceManager.getService(Context.TEXT_SERVICES_MANAGER_SERVICE));
        check(text != null && !(text.asBinder() instanceof Binder), "text service is an actual BinderProxy");
        TextServicesManager manager = context.getSystemService(TextServicesManager.class);
        check(manager != null && manager == context.getSystemService(Context.TEXT_SERVICES_MANAGER_SERVICE),
                "remote-backed public text manager constructs and caches");
        check(manager.isSpellCheckerEnabled() && text.isSpellCheckerEnabled(0),
                "remote default enabled preference is independent of provider availability");
        check(manager.getCurrentSpellCheckerInfo() == null
                        && manager.getCurrentSpellCheckerSubtype(true) == null
                        && manager.getEnabledSpellCheckerInfos().isEmpty()
                        && text.getEnabledSpellCheckers(0) == null,
                "remote no-checker results retain exact Binder and public shapes");
        check(manager.newSpellCheckerSession(null, null,
                new SpellCheckerSession.SpellCheckerSessionListener() {
                    @Override public void onGetSuggestions(SuggestionsInfo[] results) {
                        throw new AssertionError("fabricated spell result");
                    }
                    @Override public void onGetSentenceSuggestions(SentenceSuggestionsInfo[] results) {
                        throw new AssertionError("fabricated sentence result");
                    }
                }, true) == null, "remote session absence returns normally");
        denied(() -> text.getEnabledSpellCheckers(10), "foreign text user was accepted");
        MediaRouterContractTest.runWithMainLoop(context, true);
        deathProbe = MediaRouterContractTest.registerDeathProbe(context);
        check(deathProbe != null, "real Binder-death probe remains registered until process exit");
        System.out.println("MediaTextRemoteTest all passed: " + checks + " checks; ordinary UID and BinderProxy");
        System.exit(0);
    }

    private static void verifyCleanup() throws Exception {
        bootstrap("media-text-verifier");
        check(Process.myUid() == Process.SYSTEM_UID, "cleanup verifier UID");
        IBinder control = ServiceManager.getService(CONTROL);
        long deadline = System.nanoTime() + java.util.concurrent.TimeUnit.SECONDS.toNanos(10);
        while (true) {
            Parcel request = Parcel.obtain(), reply = Parcel.obtain();
            boolean clean;
            try {
                request.writeInterfaceToken(CONTROL);
                check(control.transact(2, request, reply, 0), "cleanup control transaction");
                reply.readException();
                int routers = reply.readInt(), clients = reply.readInt(), processes = reply.readInt();
                clean = routers == 0 && clients == 0 && processes == 0;
                if (!clean && System.nanoTime() >= deadline) {
                    throw new AssertionError("Binder death retained routers=" + routers
                            + ", legacy clients=" + clients + ", process fixtures=" + processes);
                }
            } finally { reply.recycle(); request.recycle(); }
            if (clean) break;
            Thread.sleep(50);
        }
        System.out.println("MediaTextRemoteTest death cleanup passed");
        System.exit(0);
    }

    public static void main(String[] args) throws Exception {
        if (args[0].equals("server")) server(args[1], args[2]);
        else if (args[0].equals("verify-cleanup")) verifyCleanup();
        else client(args[1], Integer.parseInt(args[2]));
    }
}
