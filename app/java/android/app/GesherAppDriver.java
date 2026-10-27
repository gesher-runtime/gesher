/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.app;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.FeatureInfo;
import android.content.pm.SystemFeaturesCache;
import android.content.res.CompatibilityInfo;
import android.content.res.Configuration;
import android.os.Binder;
import android.os.Bundle;
import android.os.LocaleList;
import android.util.ArrayMap;
import android.util.SuperNotCalledException;
import android.view.Display;

import com.gesher.app.DeviceProfile;

import dalvik.system.PathClassLoader;
import dalvik.system.VMRuntime;

import java.lang.ref.WeakReference;
import java.lang.reflect.Field;

/**
 * Starts an app and drives its Activity to RESUMED, the way Ravenwood's
 * RavenwoodAppDriver and RavenwoodActivityDriver do: a scaled-down
 * ActivityThread.handleBindApplication, then performLaunchActivity, handleStartActivity
 * and handleResumeActivity without system_server's transactions, and without
 * makeVisible (no window is added while headless).
 *
 * <p>It is in {@code android.app}, and on the boot class path with framework.jar, for the
 * package-private API those steps use: ActivityThread's fields, LoadedApk,
 * ContextImpl's factories, Instrumentation.basicInit, Activity.attach and perform*. Private
 * fields with no package-private way in are set by reflection ({@link #set}).
 */
public final class GesherAppDriver {
    private static final String TAG = "gesher-app";

    /** Keeps the LoadedApk alive: ActivityThread's mPackages holds it weakly. */
    private static LoadedApk sLoadedApk;
    private static volatile Activity sActivity;
    // The launched Activity's record (what ActivityClientRecord keeps; windows3 track): its
    // token, info and title, its window's config callback (to whichever instance has the
    // token), its override configuration, the orientation it asks for and the rotation its
    // task's display is turned to.
    private static Binder sToken;
    private static ActivityInfo sInfo;
    private static CharSequence sTitle;
    private static android.view.ViewRootImpl.ActivityConfigCallback sConfigCallback;
    private static Configuration sOverride;
    private static int sRequestedOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;
    private static int sRotation = android.view.Surface.ROTATION_0;

    private GesherAppDriver() {
    }

    private static void log(String message) {
        System.out.println(TAG + ": " + message);
        System.out.flush();
    }

    /** The activity launched last (locale track: configuration changes in a single process). */
    public static Activity currentActivity() {
        return sActivity;
    }

    /**
     * Binds the application and launches {@code activityInfo}, on the main thread.
     * Returns the resumed Activity.
     */
    public static Activity launch(ApplicationInfo appInfo, ActivityInfo activityInfo)
            throws Exception {
        ActivityThread thread = bindApplication(appInfo);
        return launchActivity(thread, activityInfo);
    }

    /** handleBindApplication, scaled down. */
    private static ActivityThread bindApplication(ApplicationInfo appInfo) throws Exception {
        com.gesher.app.window.ApplicationDisplay.installClient();
        // ActivityThread.main() would construct it and attach(false) it to
        // ActivityManagerService; here the parts of attach() that set up this process.
        ActivityThread thread = new ActivityThread();
        set(ActivityThread.class, null, "sCurrentActivityThread", thread);
        ConfigurationController configController = new ConfigurationController(thread);
        set(ActivityThread.class, thread, "mConfigurationController", configController);

        // System features: ActivityThread reads them early from ApplicationSharedMemory,
        // which ActivityManagerService fills from PackageManagerService. The headless
        // device declares none (hasSystemFeature answers false for each SDK feature).
        if (!SystemFeaturesCache.hasInstance()) {
            SystemFeaturesCache.setInstance(
                    new SystemFeaturesCache(new ArrayMap<String, FeatureInfo>()));
        }

        // Process state ActivityManagerService would have sent in bindApplication.
        try {
            VMRuntime.getRuntime().setTargetSdkVersion(appInfo.targetSdkVersion);
        } catch (Throwable t) {
            log("VMRuntime.setTargetSdkVersion: " + t);
        }
        long[] disabledChanges = com.gesher.server.CompatChanges.disabledFor(appInfo.targetSdkVersion);
        AppCompatCallbacks.install(disabledChanges, new long[0]);
        // The device's, with the app's own locales (LocaleManager, locale track).
        Configuration config = com.gesher.server.LocaleManagerService.configurationFor(
                appInfo.packageName);
        CompatibilityInfo compat = CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO;
        ActivityThread.AppBindData data = new ActivityThread.AppBindData();
        data.appInfo = appInfo;
        data.processName = appInfo.processName;
        data.config = config;
        data.compatInfo = compat;
        data.disabledCompatChanges = disabledChanges;
        thread.mBoundApplication = data;

        configController.setConfiguration(config);
        configController.setCompatConfiguration(config);
        thread.mConfiguration = configController.getConfiguration();
        set(ActivityThread.class, thread, "mCompatibilityInfo", compat);
        LocaleList.setDefault(config.getLocales());
        ResourcesManager.getInstance().applyConfigurationToResources(config, compat);
        thread.mCurDefaultDisplayDpi = config.densityDpi;
        configController.updateDefaultDensity(config.densityDpi);

        // The app's LoadedApk, as getPackageInfo() makes it, with the class loader
        // installed directly: LoadedApk's own (ApplicationLoaders) also creates a linker
        // namespace for the app's native libraries (ClassLoaderFactory's native
        // createClassloaderNamespace), which Gesher does not have yet.
        LoadedApk apk = new LoadedApk(thread, appInfo, compat, null /* baseLoader */,
                false /* securityViolation */, true /* includeCode */,
                false /* registerPackage */);
        ClassLoader loader = new PathClassLoader(appInfo.sourceDir, appInfo.nativeLibraryDir,
                String.class.getClassLoader() /* the boot class loader */);
        set(LoadedApk.class, apk, "mDefaultClassLoader", loader);
        set(LoadedApk.class, apk, "mClassLoader", loader);
        Thread.currentThread().setContextClassLoader(loader);
        synchronized (ResourcesManager.getInstance()) {
            thread.mPackages.put(appInfo.packageName, new WeakReference<>(apk));
        }
        data.info = apk;
        sLoadedApk = apk;
        // The app's code path for the JIT's profile saver, as LoadedApk's
        // registerAppInfoToArt registers it when it makes its class loader (ADR 0054).
        if ((appInfo.flags & ApplicationInfo.FLAG_HAS_CODE) != 0) {
            int user = android.os.UserHandle.myUserId();
            dalvik.system.VMRuntime.registerAppInfo(appInfo.packageName,
                    android.content.pm.dex.ArtManager.getCurrentProfilePath(appInfo.packageName,
                            user, null),
                    android.content.pm.dex.ArtManager.getReferenceProfilePath(appInfo.packageName,
                            user, null),
                    new String[] {appInfo.sourceDir},
                    dalvik.system.VMRuntime.CODE_PATH_TYPE_PRIMARY_APK);
        }

        // Instrumentation, plain (no instrumentation package): basicInit(thread).
        Instrumentation instrumentation = new Instrumentation();
        instrumentation.basicInit(thread);
        thread.mInstrumentation = instrumentation;

        // The app's network security config (cleartext policy, trust anchors), installed
        // before any app code runs, as handleBindApplication does (docs/network.md).
        try {
            android.security.net.config.NetworkSecurityConfigProvider.install(
                    ContextImpl.createAppContext(thread, apk));
        } catch (Throwable t) {
            log("NetworkSecurityConfigProvider.install: " + t);
        }

        // The Application, then its onCreate().
        Application app = apk.makeApplicationInner(false /* forceDefaultAppClass */,
                null /* instrumentation */);
        thread.mInitialApplication = app;
        instrumentation.onCreate(null);
        instrumentation.callApplicationOnCreate(app);
        log(app.getClass().getName() + " onCreate");
        return thread;
    }

    /** performLaunchActivity, then handleStartActivity and handleResumeActivity. */
    private static Activity launchActivity(ActivityThread thread, ActivityInfo info)
            throws Exception {
        LoadedApk apk = sLoadedApk;
        Instrumentation instrumentation = thread.mInstrumentation;
        Application app = thread.mInitialApplication;
        ComponentName component = new ComponentName(info.packageName, info.name);
        Intent intent = new Intent(Intent.ACTION_MAIN)
                .addCategory(Intent.CATEGORY_LAUNCHER)
                .setComponent(component)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        Binder token = new Binder();
        // Its orientation requests and moveTaskToBack, which no system server answers in
        // this process (windows3 track): the window's ActivityClientHook asks here.
        final android.os.Handler main = new android.os.Handler(android.os.Looper.getMainLooper());
        com.gesher.app.window.ActivityClientHook.setLocal(token,
                new com.gesher.app.window.ActivityClientHook.Local() {
                    @Override
                    public void setRequestedOrientation(android.os.IBinder t, int orientation) {
                        // After the call, as the server's transaction would come (the call
                        // may be in onCreate).
                        main.post(() -> GesherAppDriver.setRequestedOrientation(orientation));
                    }

                    @Override
                    public int getRequestedOrientation(android.os.IBinder t) {
                        return GesherAppDriver.getRequestedOrientation();
                    }
                });

        // The orientation it asks for turns its task's display (windows3 track): it starts
        // with the turned display's configuration, as the system server would launch it.
        sRequestedOrientation = info.screenOrientation;
        sRotation = com.gesher.app.window.TaskConfiguration.rotationFor(info.screenOrientation,
                android.view.Surface.ROTATION_0);
        com.gesher.app.window.ApplicationDisplay.update(sRotation);
        Configuration override = taskOverride(sRotation);
        sOverride = override;
        if (override != null) {
            log(info.name + " asks for orientation "
                    + ActivityInfo.screenOrientationToString(info.screenOrientation)
                    + ": its task's display turned to "
                    + com.gesher.app.window.TaskConfiguration.rotationName(sRotation));
        }
        ContextImpl context = ContextImpl.createActivityContext(thread, apk, info, token,
                Display.DEFAULT_DISPLAY, override);
        ClassLoader cl = context.getClassLoader();
        Activity activity = instrumentation.newActivity(cl, component.getClassName(), intent);
        intent.setExtrasClassLoader(cl);
        sActivity = activity;

        CharSequence title = info.labelRes != 0 ? app.getResources().getText(info.labelRes)
                : info.nonLocalizedLabel != null ? info.nonLocalizedLabel
                : info.applicationInfo.labelRes != 0
                        ? app.getResources().getText(info.applicationInfo.labelRes)
                        : info.name;
        Configuration config = new Configuration(
                thread.mConfiguration != null ? thread.mConfiguration
                        : DeviceProfile.configuration());
        if (override != null) config.updateFrom(override);
        sToken = token;
        sInfo = info;
        sTitle = title;

        context.setOuterContext(activity);
        // The configuration its window gets (a resized window: docs/lifecycle.md) reaches the
        // Activity as ActivityThread's activity config callback takes it there: to the
        // instance that has the token now (a relaunch makes a new one).
        android.view.ViewRootImpl.ActivityConfigCallback configCallback =
                new android.view.ViewRootImpl.ActivityConfigCallback() {
                    @Override
                    public void onConfigurationChanged(Configuration overrideConfig,
                            int displayId) {
                        configurationChanged(sActivity, token, info, overrideConfig, displayId);
                    }
                };
        sConfigCallback = configCallback;
        activity.attach(context, thread, instrumentation, token, 0 /* ident */, app, intent,
                info, title, null /* parent */, null /* embeddedID */,
                null /* lastNonConfigurationInstances */, config, null /* referrer */,
                null /* voiceInteractor */, null /* window */,
                configCallback, null /* assistToken */,
                null /* shareableActivityToken */, null /* initialCallerInfoAccessToken */);
        log(info.name + " attach");
        activity.mStartedActivity = false;
        int theme = info.getThemeResource();
        if (theme != 0) {
            activity.setTheme(theme);
        }

        // ON_CREATE
        activity.mCalled = false;
        instrumentation.callActivityOnCreate(activity, (Bundle) null);
        if (!activity.mCalled) {
            throw new SuperNotCalledException("Activity " + component.toShortString()
                    + " did not call through to super.onCreate()");
        }
        log(info.name + " onCreate");

        // ON_START, and the post-create step handleStartActivity does
        activity.performStart("gesher-app");
        log(info.name + " onStart");
        activity.mCalled = false;
        instrumentation.callActivityOnPostCreate(activity, (Bundle) null);
        if (!activity.mCalled) {
            throw new SuperNotCalledException("Activity " + component.toShortString()
                    + " did not call through to super.onPostCreate()");
        }

        // ON_RESUME. handleResumeActivity would now add the decor view to the window
        // manager and makeVisible(); headless, the Activity stays resumed and unshown.
        activity.performResume(false /* followedByPause */, "gesher-app");
        log(info.name + " onResume");
        // The only Activity: the top resumed one (Android 10's onTopResumedActivityChanged).
        activity.performTopResumedActivityChanged(true, "gesher-app");
        return activity;
    }

    /** Every task has an explicit display rotation, including the natural zero. */
    private static Configuration taskOverride(int rotation) {
        return com.gesher.app.window.TaskConfiguration.override(
                com.gesher.app.window.TaskConfiguration.display(rotation), rotation);
    }

    /**
     * Activity.setRequestedOrientation in the single process (windows3 track; the window's
     * ActivityClientHook asks): the orientation turns the task's display as the system
     * server turns a task's (TaskConfiguration.rotationFor); the Activity gets the turned
     * display's configuration as a configuration change (told if it handles it, else
     * relaunched), its window takes the turned shape (WindowSession). On the main thread,
     * after the call, as the server's transaction would come.
     */
    public static void setRequestedOrientation(int orientation) {
        Activity a = sActivity;
        if (a == null || sToken == null) return;
        sRequestedOrientation = orientation;
        int rotation = com.gesher.app.window.TaskConfiguration.rotationFor(orientation, sRotation);
        if (rotation == sRotation) {
            log(sInfo.name + " asks for orientation "
                    + ActivityInfo.screenOrientationToString(orientation)
                    + ": its task stays as it is");
            return;
        }
        log(sInfo.name + " asks for orientation "
                + ActivityInfo.screenOrientationToString(orientation) + ": its task's display"
                + " turned from " + com.gesher.app.window.TaskConfiguration.rotationName(sRotation)
                + " to " + com.gesher.app.window.TaskConfiguration.rotationName(rotation));
        sRotation = rotation;
        com.gesher.app.window.ApplicationDisplay.update(sRotation);
        Configuration override = taskOverride(rotation);
        configurationChanged(a, sToken, sInfo, override != null ? override : new Configuration(),
                Display.DEFAULT_DISPLAY);
    }

    /** What setRequestedOrientation set last (else the manifest's). */
    public static int getRequestedOrientation() {
        return sRequestedOrientation;
    }

    /**
     * ActivityThread.handleRelaunchActivity with its window preserved, scaled down (windows3
     * track: the single process relaunches its Activity for configuration changes it does not
     * handle, as the system server has the device's relaunched): onPause, onStop,
     * onSaveInstanceState, onDestroy (isChangingConfigurations, its non-configuration
     * instance kept), then a new instance with the same token in the same window (its content
     * cleared: the decor view, its ViewRootImpl and Win32 window stay) and the new
     * configuration: onCreate with the saved state, onStart, onRestoreInstanceState,
     * onPostCreate, onResume. Returns the new instance.
     */
    private static Activity relaunch(Activity old, Configuration overrideConfig, int changes)
            throws Exception {
        ActivityThread thread = ActivityThread.currentActivityThread();
        Instrumentation instrumentation = thread.mInstrumentation;
        Application app = thread.mInitialApplication;
        ActivityInfo info = sInfo;
        boolean resumed = old.isResumed();
        boolean stopped = old.mStopped;
        Intent intent = old.getIntent();
        log(info.name + " relaunching for configuration changes 0x"
                + Integer.toHexString(changes) + " it does not handle (single process)");
        old.mConfigChangeFlags |= changes;
        old.mChangingConfigurations = true;
        if (resumed) {
            old.performTopResumedActivityChanged(false, "relaunch");
            old.performPause();
        }
        Bundle state = new Bundle();
        boolean preP = info.applicationInfo.targetSdkVersion < android.os.Build.VERSION_CODES.P;
        if (!stopped) {
            if (preP) instrumentation.callActivityOnSaveInstanceState(old, state);
            old.performStop(true /* preserveWindow */, "relaunch");
            if (!preP) instrumentation.callActivityOnSaveInstanceState(old, state);
        } else {
            instrumentation.callActivityOnSaveInstanceState(old, state);
        }
        Activity.NonConfigurationInstances retained = old.retainNonConfigurationInstances();
        android.view.Window window = old.getWindow();
        android.view.View decor = old.mDecor;
        instrumentation.callActivityOnDestroy(old);
        // Only the part of the view hierarchy the framework controls stays.
        if (window != null) window.clearContentView();
        old.mDecor = null;

        // The new instance (performLaunchActivity with r.mPendingRemoveWindow).
        sOverride = overrideConfig;
        ContextImpl context = ContextImpl.createActivityContext(thread, sLoadedApk, info, sToken,
                Display.DEFAULT_DISPLAY, overrideConfig);
        ClassLoader cl = context.getClassLoader();
        Activity activity = instrumentation.newActivity(cl, info.name, intent);
        intent.setExtrasClassLoader(cl);
        Configuration config = new Configuration(thread.mConfiguration != null
                ? thread.mConfiguration : DeviceProfile.configuration());
        if (overrideConfig != null) config.updateFrom(overrideConfig);
        context.setOuterContext(activity);
        activity.attach(context, thread, instrumentation, sToken, 0 /* ident */, app, intent,
                info, sTitle, null /* parent */, null /* embeddedID */, retained, config,
                null /* referrer */, null /* voiceInteractor */, window /* preserved */,
                sConfigCallback, null /* assistToken */, null /* shareableActivityToken */,
                null /* initialCallerInfoAccessToken */);
        activity.mStartedActivity = false;
        int theme = info.getThemeResource();
        if (theme != 0) activity.setTheme(theme);
        sActivity = activity;
        activity.mCalled = false;
        instrumentation.callActivityOnCreate(activity, state);
        if (!activity.mCalled) {
            throw new SuperNotCalledException("Activity " + info.name
                    + " did not call through to super.onCreate()");
        }
        log(info.name + " onCreate (relaunched)");
        activity.performStart("gesher-app relaunch");
        instrumentation.callActivityOnRestoreInstanceState(activity, state);
        activity.mCalled = false;
        instrumentation.callActivityOnPostCreate(activity, state);
        if (!activity.mCalled) {
            throw new SuperNotCalledException("Activity " + info.name
                    + " did not call through to super.onPostCreate()");
        }
        // handleResumeActivity with r.mPreserveWindow: the decor view is the window's already.
        if (decor != null) {
            activity.mDecor = decor;
            activity.mWindowAdded = true;
            android.view.WindowManager.LayoutParams l = activity.getWindow().getAttributes();
            l.type = android.view.WindowManager.LayoutParams.TYPE_BASE_APPLICATION;
            android.view.ViewRootImpl impl = decor.getViewRootImpl();
            if (impl != null) {
                impl.notifyChildRebuilt();
                impl.setActivityConfigCallback(sConfigCallback);
            }
        }
        if (resumed) {
            activity.performResume(false /* followedByPause */, "gesher-app relaunch");
            log(info.name + " onResume (relaunched)");
            activity.performTopResumedActivityChanged(true, "gesher-app relaunch");
        } else if (stopped) {
            // It was stopped (its window minimized): it ends stopped again.
            activity.performStop(true /* preserveWindow */, "gesher-app relaunch");
        }
        if (decor != null && !stopped) {
            activity.mVisibleFromServer = true;
            if (activity.mVisibleFromClient) {
                activity.onWindowAttributesChanged(activity.getWindow().getAttributes());
                activity.makeVisible();
            }
        }
        return activity;
    }

    private static boolean sRelaunchPending;
    private static int sRelaunchChanges;

    /**
     * ActivityThread.handleActivityConfigurationChanged, scaled down: the Activity's
     * resources take its window's override configuration (ResourcesManager), it gets
     * onConfigurationChanged for the changes it handles (android:configChanges); the
     * ones it does not handle relaunch it (windows3 track: {@link #relaunch}, from the main
     * Looper, as the server's ActivityRelaunchItem comes); then its window updates
     * (ViewRootImpl.updateConfiguration).
     */
    private static void configurationChanged(Activity activity, Binder token, ActivityInfo info,
            Configuration overrideConfig, int displayId) {
        if (activity == null) return;
        Configuration old = new Configuration(activity.getResources().getConfiguration());
        ResourcesManager.getInstance().updateResourcesForActivity(token, overrideConfig,
                Display.DEFAULT_DISPLAY);
        sOverride = overrideConfig;
        Configuration now = new Configuration(activity.getResources().getConfiguration());
        int diff = old.diff(now) & ~(ActivityInfo.CONFIG_WINDOW_CONFIGURATION
                | ActivityInfo.CONFIG_ASSETS_PATHS);
        if (diff != 0) {
            int unhandled = diff & ~info.getRealConfigChanged();
            activity.mCurrentConfig = new Configuration(now);
            if (unhandled == 0) {
                activity.mCalled = false;
                activity.onConfigurationChanged(new Configuration(now));
                if (!activity.mCalled) {
                    throw new SuperNotCalledException("Activity " + info.name
                            + " did not call through to super.onConfigurationChanged()");
                }
            } else {
                sRelaunchChanges |= unhandled;
                if (!sRelaunchPending) {
                    sRelaunchPending = true;
                    new android.os.Handler(android.os.Looper.getMainLooper()).post(() -> {
                        sRelaunchPending = false;
                        int changes = sRelaunchChanges;
                        sRelaunchChanges = 0;
                        Activity current = sActivity;
                        if (current == null || current.mFinished || current.isDestroyed()) {
                            return;
                        }
                        try {
                            relaunch(current, sOverride, changes);
                        } catch (Exception e) {
                            throw new RuntimeException("relaunching " + info.name, e);
                        }
                    });
                }
            }
        }
        android.view.View decor = activity.mDecor;
        android.view.ViewRootImpl root = decor != null ? decor.getViewRootImpl() : null;
        if (root != null) root.updateConfiguration(displayId);
    }

    /** Sets a private field: the ones ActivityThread.attach() and LoadedApk set. */
    private static void set(Class<?> c, Object target, String name, Object value)
            throws ReflectiveOperationException {
        Field f = c.getDeclaredField(name);
        f.setAccessible(true);
        f.set(target, value);
    }
}
