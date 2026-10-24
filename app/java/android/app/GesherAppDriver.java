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
    private static Activity sActivity;

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
        // The device's, with the app's own locales (LocaleManager, locale track).
        Configuration config = com.gesher.server.LocaleManagerService.configurationFor(
                appInfo.packageName);
        CompatibilityInfo compat = CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO;
        ActivityThread.AppBindData data = new ActivityThread.AppBindData();
        data.appInfo = appInfo;
        data.processName = appInfo.processName;
        data.config = config;
        data.compatInfo = compat;
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

        // Instrumentation, plain (no instrumentation package): basicInit(thread).
        Instrumentation instrumentation = new Instrumentation();
        instrumentation.basicInit(thread);
        thread.mInstrumentation = instrumentation;

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

        ContextImpl context = ContextImpl.createActivityContext(thread, apk, info, token,
                Display.DEFAULT_DISPLAY, null /* overrideConfiguration */);
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

        context.setOuterContext(activity);
        activity.attach(context, thread, instrumentation, token, 0 /* ident */, app, intent,
                info, title, null /* parent */, null /* embeddedID */,
                null /* lastNonConfigurationInstances */, config, null /* referrer */,
                null /* voiceInteractor */, null /* window */,
                null /* activityConfigCallback */, null /* assistToken */,
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
        return activity;
    }

    /** Sets a private field: the ones ActivityThread.attach() and LoadedApk set. */
    private static void set(Class<?> c, Object target, String name, Object value)
            throws ReflectiveOperationException {
        Field f = c.getDeclaredField(name);
        f.setAccessible(true);
        f.set(target, value);
    }
}
