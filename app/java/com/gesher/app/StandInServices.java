/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;

import android.app.ContentProviderHolder;
import android.app.IActivityClientController;
import android.app.IActivityManager;
import android.app.IActivityTaskManager;
import android.app.IApplicationThread;
import android.content.ComponentName;
import android.content.Context;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.UserInfo;
import android.graphics.Point;
import android.hardware.display.IDisplayManager;
import android.hardware.display.IDisplayManagerCallback;
import android.os.IBinder;
import android.os.IServiceManager;
import android.os.IUserManager;
import android.os.Service;
import android.os.ServiceManager;
import android.os.ServiceWithMetadata;
import android.view.Display;
import android.view.DisplayInfo;
import android.view.IWindowManager;
import android.view.IWindowSession;
import android.view.IWindowSessionCallback;

import java.lang.reflect.Field;
import java.util.Map;
import java.util.TreeMap;

/**
 * The system services an app process looks up through {@link ServiceManager}, as
 * in-process stand-ins (Ravenwood's RavenwoodSystemServer does the same for its tests).
 * Each overrides only what the headless lifecycle calls; see {@link StandIn} for the
 * rest. There is no system_server: these answer for one app, the one being launched.
 */
public final class StandInServices {
    private static final Map<String, IBinder> sServices = new TreeMap<>();

    private StandInServices() {
    }

    static void install(AppManifest app) throws ReflectiveOperationException {
        add(Context.ACTIVITY_SERVICE,
                StandIn.of(IActivityManager.class, new ActivityManager()));
        add(Context.ACTIVITY_TASK_SERVICE,
                StandIn.of(IActivityTaskManager.class, new ActivityTaskManager()));
        add("package", StandIn.of(IPackageManager.class, new PackageManagerService(app)));
        add(Context.WINDOW_SERVICE, StandIn.of(IWindowManager.class, new WindowManager()));
        add(Context.DISPLAY_SERVICE, StandIn.of(IDisplayManager.class, new DisplayManager()));
        add(Context.USER_SERVICE, StandIn.of(IUserManager.class, new UserManager()));

        // The service manager itself: ServiceManager asks it for anything not in its
        // cache. Without a binder driver BinderInternal.getContextObject() has no
        // servicemanager to return, so the stand-in replaces the proxy it would build.
        StandIn<IServiceManager> sm =
                StandIn.of(IServiceManager.class, new ServiceManagerStandIn());
        Field f = ServiceManager.class.getDeclaredField("sServiceManager");
        f.setAccessible(true);
        f.set(null, sm.asInterface());

        // What bindApplication hands an app process on a device.
        ServiceManager.initServiceCache(new TreeMap<>(sServices));
        Main.log("stand-in services: " + sServices.keySet());
    }

    private static void add(String name, StandIn<?> service) {
        sServices.put(name, service.asBinder());
    }

    /** servicemanager: the stand-ins above, and nothing else. */
    public static final class ServiceManagerStandIn extends IServiceManager.Default {
        @Override
        public IBinder getService(String name) {
            return sServices.get(name);
        }

        @Override
        public IBinder checkService(String name) {
            return sServices.get(name);
        }

        @Override
        public Service getService2(String name) {
            ServiceWithMetadata s = new ServiceWithMetadata();
            s.service = sServices.get(name);
            return Service.serviceWithMetadata(s);
        }

        @Override
        public Service checkService2(String name) {
            return getService2(name);
        }

        @Override
        public void addService(String name, IBinder service, boolean allowIsolated,
                int dumpPriority) {
            sServices.put(name, service);
        }

        @Override
        public String[] listServices(int dumpPriority) {
            return sServices.keySet().toArray(new String[0]);
        }

        @Override
        public boolean isDeclared(String name) {
            return false;
        }
    }

    /** activity: ActivityManagerService. */
    public static final class ActivityManager extends IActivityManager.Default {
        @Override
        public ContentProviderHolder getContentProvider(IApplicationThread caller,
                String callingPackage, String name, int userId, boolean stable) {
            return null;  // No providers yet: Settings and friends fall back to defaults.
        }

        @Override
        public int checkPermission(String permission, int pid, int uid) {
            return PackageManager.PERMISSION_GRANTED;
        }

        @Override
        public UserInfo getCurrentUser() {
            return UserManager.owner();
        }
    }

    /** activity_task: ActivityTaskManagerService, and its per-activity controller. */
    public static final class ActivityTaskManager extends IActivityTaskManager.Default {
        private final StandIn<IActivityClientController> mClient =
                StandIn.of(IActivityClientController.class, new ActivityClientController());

        @Override
        public IActivityClientController getActivityClientController() {
            return mClient.asInterface();
        }
    }

    public static final class ActivityClientController
            extends IActivityClientController.Default {
        @Override
        public int getDisplayId(IBinder activityToken) {
            return Display.DEFAULT_DISPLAY;
        }

        @Override
        public int getTaskForActivity(IBinder token, boolean onlyRoot) {
            return 1;
        }
    }

    /** package: PackageManagerService, knowing one package. */
    public static final class PackageManagerService extends IPackageManager.Default {
        private final AppManifest mApp;

        PackageManagerService(AppManifest app) {
            mApp = app;
        }

        private boolean isApp(String packageName) {
            return mApp.applicationInfo.packageName.equals(packageName);
        }

        @Override
        public PackageInfo getPackageInfo(String packageName, long flags, int userId) {
            return isApp(packageName) ? mApp.packageInfo() : null;
        }

        @Override
        public ApplicationInfo getApplicationInfo(String packageName, long flags,
                int userId) {
            return isApp(packageName) ? new ApplicationInfo(mApp.applicationInfo) : null;
        }

        @Override
        public ActivityInfo getActivityInfo(ComponentName className, long flags, int userId) {
            if (!isApp(className.getPackageName())) {
                return null;
            }
            ActivityInfo a = mApp.findActivity(className.getClassName());
            return a == null ? null : new ActivityInfo(a);
        }

        @Override
        public boolean hasSystemFeature(String name, int version) {
            return false;
        }

        @Override
        public String getNameForUid(int uid) {
            return mApp.applicationInfo.packageName;
        }

        @Override
        public String[] getPackagesForUid(int uid) {
            return new String[] {mApp.applicationInfo.packageName};
        }

        @Override
        public String getInstallerPackageName(String packageName) {
            return null;
        }
    }

    /** window: WindowManagerService. No windows are added while headless. */
    public static final class WindowManager extends IWindowManager.Default {
        private final StandIn<IWindowSession> mSession =
                StandIn.of(IWindowSession.class, new IWindowSession.Default());

        @Override
        public IWindowSession openSession(IWindowSessionCallback callback) {
            return mSession.asInterface();
        }

        @Override
        public void getInitialDisplaySize(int displayId, Point size) {
            size.set(DeviceProfile.WIDTH, DeviceProfile.HEIGHT);
        }

        @Override
        public void getBaseDisplaySize(int displayId, Point size) {
            size.set(DeviceProfile.WIDTH, DeviceProfile.HEIGHT);
        }

        @Override
        public boolean hasNavigationBar(int displayId) {
            return false;
        }

        @Override
        public float getCurrentAnimatorScale() {
            return 1f;
        }

        @Override
        public float[] getAnimationScales() {
            return new float[] {1f, 1f, 1f};
        }
    }

    /** display: DisplayManagerService, with one display. */
    public static final class DisplayManager extends IDisplayManager.Default {
        @Override
        public DisplayInfo getDisplayInfo(int displayId) {
            return displayId == Display.DEFAULT_DISPLAY ? DeviceProfile.displayInfo() : null;
        }

        @Override
        public int[] getDisplayIds(boolean includeDisabled) {
            return new int[] {Display.DEFAULT_DISPLAY};
        }

        @Override
        public void registerCallback(IDisplayManagerCallback callback) {
            // The display never changes.
        }

        @Override
        public void registerCallbackWithEventMask(IDisplayManagerCallback callback,
                long eventsMask) {
        }

        @Override
        public Point getStableDisplaySize() {
            return new Point(DeviceProfile.WIDTH, DeviceProfile.HEIGHT);
        }
    }

    /** user: UserManagerService, with the system user, running and unlocked. */
    public static final class UserManager extends IUserManager.Default {
        static UserInfo owner() {
            return new UserInfo(0, "Owner",
                    UserInfo.FLAG_ADMIN | UserInfo.FLAG_FULL | UserInfo.FLAG_SYSTEM);
        }

        @Override
        public UserInfo getUserInfo(int userId) {
            return userId == 0 ? owner() : null;
        }

        @Override
        public boolean isUserUnlocked(int userId) {
            return userId == 0;
        }

        @Override
        public boolean isUserUnlockingOrUnlocked(int userId) {
            return userId == 0;
        }

        @Override
        public boolean isUserRunning(int userId) {
            return userId == 0;
        }
    }
}
