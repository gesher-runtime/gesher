/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.ActivityThread;
import android.app.Application;
import android.app.LoadedApk;
import android.content.Context;
import android.content.pm.IPackageManager;
import android.content.pm.SystemFeaturesCache;
import android.content.res.AssetManager;
import android.content.res.CompatibilityInfo;
import android.hardware.display.DisplayManager;
import android.hardware.display.WifiDisplayStatus;
import android.media.AudioManager;
import android.media.IMediaRouter2;
import android.media.IMediaRouterService;
import android.media.MediaRoute2Info;
import android.media.MediaRouter;
import android.media.MediaRouter2;
import android.media.RouteDiscoveryPreference;
import android.media.RoutingChangeInfo;
import android.media.RoutingSessionInfo;
import android.media.SuggestedDeviceInfo;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Looper;
import android.os.Parcel;
import android.os.Process;
import android.os.RemoteException;
import android.os.ServiceManager;

import com.gesher.server.Services;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Actual public routers, route/session consistency, callback completion and stream volume. */
public final class MediaRouterContractTest {
    private static int checks;
    private static boolean featuresInitialized;
    private static synchronized void check(boolean condition, String reason) {
        if (!condition) throw new AssertionError(reason);
        checks++;
    }

    private static Field field(Class<?> type, String name) throws Exception {
        Field result = type.getDeclaredField(name); result.setAccessible(true); return result;
    }

    /** ContextImpl plus an attached Application; neither app code nor an activity is launched. */
    public static synchronized Context context(String packageName) throws Exception {
        Constructor<ActivityThread> constructor = ActivityThread.class.getDeclaredConstructor();
        constructor.setAccessible(true);
        ActivityThread thread = constructor.newInstance();
        field(ActivityThread.class, "sCurrentActivityThread").set(null, thread);
        Context context = thread.getSystemContext();
        if (packageName != null) {
            LoadedApk packageInfo = thread.getPackageInfo(packageName,
                    CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO, 0);
            check(packageInfo != null, "fixture package resolves through PackageManager");
            // createPackageContext retains its parent's Binder/AppOps identity.
            // Use the same app-context factory as ActivityThread application binding,
            // so an ordinary client never registers receivers as package "android".
            Method createAppContext = Class.forName("android.app.ContextImpl").getDeclaredMethod(
                    "createAppContext", ActivityThread.class, LoadedApk.class);
            createAppContext.setAccessible(true);
            context = (Context) createAppContext.invoke(null, thread, packageInfo);
            check(packageName.equals(context.getPackageName())
                            && packageName.equals(context.getBasePackageName())
                            && packageName.equals(context.getOpPackageName()),
                    "application context uses its own package for Binder and AppOps");
            check(context.getApplicationInfo().uid == Process.myUid(),
                    "application context belongs to the ordinary client UID");
        }
        Application application = new Application();
        Method attach = Application.class.getDeclaredMethod("attach", Context.class);
        attach.setAccessible(true); attach.invoke(application, context);
        field(ActivityThread.class, "mInitialApplication").set(thread, application);
        Object loaded = field(context.getClass(), "mPackageInfo").get(context);
        field(loaded.getClass(), "mApplication").set(loaded, application);
        // Like application binding, initialize the process-wide cache once. A
        // fresh Context/LoadedApk must not bootstrap this singleton again.
        if (!featuresInitialized) {
            IPackageManager packages = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
            SystemFeaturesCache.setInstance(new SystemFeaturesCache(
                    packages.getSystemAvailableFeatures().getList()));
            featuresInitialized = true;
        }
        return context;
    }

    private static void await(CountDownLatch latch, String reason) throws Exception {
        check(latch.await(10, TimeUnit.SECONDS), reason);
    }

    private static final class SessionResult {
        final int id;
        final RoutingSessionInfo session;
        SessionResult(int id, RoutingSessionInfo session) { this.id = id; this.session = session; }
    }

    private static final class Router extends IMediaRouter2.Stub {
        final IMediaRouterService service;
        final String packageName;
        final boolean remote;
        final CountDownLatch registered = new CountDownLatch(1);
        final BlockingQueue<List<MediaRoute2Info>> updates = new LinkedBlockingQueue<>();
        final BlockingQueue<SessionResult> sessions = new LinkedBlockingQueue<>();
        final AtomicReference<Throwable> failure = new AtomicReference<>();
        volatile RoutingSessionInfo initialSession;
        volatile List<MediaRoute2Info> initialRoutes;
        Router(IMediaRouterService service, String packageName, boolean remote) {
            this.service = service; this.packageName = packageName; this.remote = remote;
        }
        private void inspect() {
            try {
                if (remote) check(Binder.getCallingUid() == Process.SYSTEM_UID,
                        "remote routing callback uses system identity");
                check(service.getSystemRoutes(packageName, false).size() == 1,
                        "callback can reenter the service without a lock deadlock");
            } catch (Throwable error) { failure.compareAndSet(null, error); }
        }
        @Override public void notifyRouterRegistered(List<MediaRoute2Info> routes,
                RoutingSessionInfo session) {
            inspect(); initialRoutes = routes; initialSession = session; registered.countDown();
        }
        @Override public void notifyRoutesUpdated(List<MediaRoute2Info> routes) {
            inspect(); updates.add(routes);
        }
        @Override public void notifySessionCreated(int id, RoutingSessionInfo session) {
            inspect(); sessions.add(new SessionResult(id, session));
        }
        @Override public void notifySessionInfoChanged(RoutingSessionInfo session) {}
        @Override public void notifySessionReleased(RoutingSessionInfo session) {
            failure.compareAndSet(null, new AssertionError("system session was released"));
        }
        @Override public void requestCreateSessionByManager(long id, RoutingSessionInfo old,
                MediaRoute2Info route, RoutingChangeInfo change) {
            failure.compareAndSet(null, new AssertionError("unavailable manager created a request"));
        }
        @Override public void notifyDeviceSuggestionsUpdated(String name,
                List<SuggestedDeviceInfo> suggestions) {}
        @Override public void notifyDeviceSuggestionRequested() {}
    }

    public static void contracts(Context context, boolean remote) throws Exception {
        IMediaRouterService service = IMediaRouterService.Stub.asInterface(
                ServiceManager.getService(Context.MEDIA_ROUTER_SERVICE));
        check(service != null, "media_router service exists");
        if (remote) check(!(service.asBinder() instanceof Binder), "actual remote BinderProxy");
        WifiDisplayStatus wifi = context.getSystemService(DisplayManager.class).getWifiDisplayStatus();
        check(wifi != null && wifi.getFeatureState() == WifiDisplayStatus.FEATURE_STATE_UNAVAILABLE
                        && wifi.getScanState() == WifiDisplayStatus.SCAN_STATE_NOT_SCANNING
                        && wifi.getActiveDisplay() == null && wifi.getDisplays().length == 0,
                "Wi-Fi display uses Android's unavailable value");
        MediaRouter legacy = context.getSystemService(MediaRouter.class);
        check(legacy != null && legacy == context.getSystemService(Context.MEDIA_ROUTER_SERVICE),
                "real ContextImpl constructs and caches the legacy router");
        check(legacy.getRouteCount() == 1 && legacy.getDefaultRoute().isDefault()
                        && legacy.getSelectedRoute(MediaRouter.ROUTE_TYPE_LIVE_AUDIO).isDefault(),
                "legacy router selects only its real local default route");
        AudioManager audio = context.getSystemService(AudioManager.class);
        int originalVolume = audio.getStreamVolume(AudioManager.STREAM_MUSIC);
        int targetVolume = originalVolume == 2 ? 3 : 2;

        MediaRouter2 router = MediaRouter2.getInstance(context);
        MediaRouter2.RoutingController controller = router.getSystemController();
        check(controller != null && controller.getSelectedRoutes().size() == 1,
                "public MediaRouter2 constructs with one selected route");
        MediaRoute2Info route = controller.getSelectedRoutes().get(0);
        check(route.isSystemRoute() && route.getType() == MediaRoute2Info.TYPE_UNKNOWN
                        && route.getFeatures().contains(MediaRoute2Info.FEATURE_LOCAL_PLAYBACK),
                "default route does not invent hardware identity or remote playback");
        check(controller.getVolumeHandling() == MediaRoute2Info.PLAYBACK_VOLUME_FIXED,
                "persistent system session does not advertise group-volume control");
        RoutingSessionInfo systemSession = service.getSystemSessionInfo();
        check(systemSession.isSystemSession() && systemSession.getSelectedRoutes().equals(
                        Collections.singletonList(route.getId()))
                        && systemSession.getReleaseType() == RoutingSessionInfo.RELEASE_UNSUPPORTED,
                "provider-qualified route and session IDs agree");
        check(!router.showSystemOutputSwitcher(), "unavailable Android output switcher reports false");
        CountDownLatch publicRoutes = new CountDownLatch(1);
        MediaRouter2.RouteCallback routeCallback = new MediaRouter2.RouteCallback() {
            @Override public void onRoutesUpdated(List<MediaRoute2Info> values) {
                if (values.size() == 1 && values.get(0).getId().equals(route.getId())) publicRoutes.countDown();
            }
        };
        Router raw = new Router(service, context.getPackageName(), remote);
        router.registerRouteCallback(Runnable::run, routeCallback,
                new RouteDiscoveryPreference.Builder(
                        Collections.singletonList(MediaRoute2Info.FEATURE_LIVE_AUDIO), false).build());
        service.registerRouter2(raw, context.getPackageName());
        try {
            await(publicRoutes, "public route callback receives registered default route");
            await(raw.registered, "raw registration callback completes");
            check(raw.initialRoutes.size() == 1 && raw.initialSession.getSelectedRoutes().equals(
                            Collections.singletonList(raw.initialRoutes.get(0).getId()))
                            && context.getPackageName().equals(raw.initialSession.getClientPackageName()),
                    "registration carries coherent package-specific system session");
            service.registerRouter2(raw, context.getPackageName()); // Idempotent.
            check(service.getDeviceSuggestionsWithRouter2(raw).isEmpty(), "no device suggestions invented");

            MediaRoute2Info unknown = new MediaRoute2Info.Builder("absent", "Absent remote route")
                    .addFeature(MediaRoute2Info.FEATURE_REMOTE_PLAYBACK).build();
            RoutingChangeInfo change = new RoutingChangeInfo(
                    RoutingChangeInfo.ENTRY_POINT_LOCAL_ROUTER_UNSPECIFIED, false);
            service.requestCreateSessionWithRouter2(raw, 71, 0, systemSession, unknown, change, null);
            SessionResult failed = raw.sessions.poll(10, TimeUnit.SECONDS);
            check(failed != null && failed.id == 71 && failed.session == null,
                    "unavailable route request completes with prescribed failure callback");
            service.requestCreateSessionWithRouter2(raw, 72, 0, systemSession, route, change, null);
            SessionResult created = raw.sessions.poll(10, TimeUnit.SECONDS);
            check(created != null && created.id == 72 && created.session != null
                            && created.session.getId().equals(systemSession.getId()),
                    "default route reuses the persistent system session");
            service.requestCreateSessionWithRouter2(raw, 73, 1, systemSession, route, change, null);
            SessionResult unauthorizedManager = raw.sessions.poll(10, TimeUnit.SECONDS);
            check(unauthorizedManager != null && unauthorizedManager.id == 73
                            && unauthorizedManager.session == null,
                    "unavailable manager request cannot create a session");

            raw.updates.clear();
            legacy.getDefaultRoute().requestSetVolume(targetVolume);
            List<MediaRoute2Info> changed = raw.updates.poll(10, TimeUnit.SECONDS);
            check(audio.getStreamVolume(AudioManager.STREAM_MUSIC) == targetVolume
                            && legacy.getDefaultRoute().getVolume() == targetVolume,
                    "legacy route volume controls actual shared music stream");
            check(changed != null && changed.get(0).getVolume() == targetVolume,
                    "AudioManager stream change notifies registered modern routes");
            int nextVolume = targetVolume + 1;
            service.setRouteVolumeWithRouter2(raw, route, nextVolume);
            check(audio.getStreamVolume(AudioManager.STREAM_MUSIC) == nextVolume
                            && service.getSystemRoutes(context.getPackageName(), false)
                                    .get(0).getVolume() == nextVolume,
                    "modern route and AudioManager share volume state");
            service.setSessionVolumeWithRouter2(raw, systemSession.getId(), 0);
            service.releaseSessionWithRouter2(raw, systemSession.getId());
            controller.release();
            check(!controller.isReleased() && audio.getStreamVolume(AudioManager.STREAM_MUSIC) == nextVolume,
                    "fixed system session persists without changing stream volume");
            if (raw.failure.get() != null) throw new AssertionError("routing callback failed", raw.failure.get());
        } finally {
            service.unregisterRouter2(raw);
            service.unregisterRouter2(raw);
            router.unregisterRouteCallback(routeCallback);
            audio.setStreamVolume(AudioManager.STREAM_MUSIC, originalVolume, 0);
        }
        System.out.println("MediaRouterContractTest all passed: " + checks + (remote ? " remote" : " local Parcel"));
    }

    private static final class EndLoop extends RuntimeException {}

    /** Keep a registration alive until this test process exits, to test real Binder death. */
    public static IBinder registerDeathProbe(Context context) throws Exception {
        IMediaRouterService service = IMediaRouterService.Stub.asInterface(
                ServiceManager.getService(Context.MEDIA_ROUTER_SERVICE));
        Router probe = new Router(service, context.getPackageName(), true);
        service.registerRouter2(probe, context.getPackageName());
        await(probe.registered, "death probe registration completes");
        if (probe.failure.get() != null) throw new AssertionError(probe.failure.get());
        return probe;
    }

    public static void runWithMainLoop(Context context, boolean remote) throws Exception {
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Handler handler = new Handler(Looper.getMainLooper());
        Thread worker = new Thread(() -> {
            try { contracts(context, remote); }
            catch (Throwable error) { failure.set(error); }
            finally { handler.post(() -> { throw new EndLoop(); }); }
        }, "media-router-contract");
        worker.start();
        try { Looper.loop(); } catch (EndLoop finished) { /* bounded by the process runner */ }
        worker.join(1000);
        if (failure.get() != null) throw new AssertionError("Media router contracts failed", failure.get());
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        if (Looper.myLooper() == null) Looper.prepareMainLooper();
        Map<String, IBinder> created = Services.createInProcess(Collections.emptyList());
        IBinder target = created.get(Context.MEDIA_ROUTER_SERVICE);
        check(target != null, "media routing is registered");
        created.put(Context.MEDIA_ROUTER_SERVICE, new Binder() {
            @Override public IInterface queryLocalInterface(String name) { return null; }
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                    throws RemoteException { return target.transact(code, data, reply, flags); }
        });
        @SuppressWarnings("unchecked") Map<String, IBinder> cache = (Map<String, IBinder>)
                field(ServiceManager.class, "sCache").get(null);
        cache.putAll(created);
        runWithMainLoop(context(null), false);
        System.exit(0);
    }
}
