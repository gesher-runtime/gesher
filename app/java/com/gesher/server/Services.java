/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.IActivityManager;
import android.app.IActivityTaskManager;
import android.content.Context;
import android.content.pm.IPackageManager;
import android.content.pm.UserInfo;
import android.graphics.Point;
import android.hardware.OverlayProperties;
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

import com.android.internal.view.IInputMethodManager;

import com.gesher.app.DeviceProfile;

import java.lang.reflect.Field;
import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.TreeMap;

/**
 * The system services an app process looks up through {@link ServiceManager}, as
 * stand-ins (Ravenwood's RavenwoodSystemServer does the same for its tests). Each
 * overrides only what the headless lifecycle calls; see {@link StandIn} for the rest.
 *
 * <p>{@link SystemServer} registers them with servicemanager, for app processes to reach
 * over Binder; {@link #installInProcess} serves them in the app's own process instead,
 * with no binder driver (gesher-app's single-process mode).
 */
public final class Services {
    private Services() {
    }

    /** The services, by name, for the given packages. */
    public static Map<String, IBinder> create(Collection<ApkPackage> packages) {
        Map<String, IBinder> services = new LinkedHashMap<>();
        PackageManagerService pm = new PackageManagerService(packages);
        ActivityTaskManagerService atm = new ActivityTaskManagerService(pm);
        ActivityManagerService am = new ActivityManagerService(atm);
        services.put("package", StandIn.of(IPackageManager.class, pm).asBinder());
        services.put(Context.ACTIVITY_SERVICE,
                StandIn.of(IActivityManager.class, am).asBinder());
        services.put(Context.ACTIVITY_TASK_SERVICE,
                StandIn.of(IActivityTaskManager.class, atm).asBinder());
        services.put(Context.WINDOW_SERVICE,
                StandIn.of(IWindowManager.class, new WindowManagerService()).asBinder());
        services.put(Context.DISPLAY_SERVICE,
                StandIn.of(IDisplayManager.class, new DisplayManager()).asBinder());
        services.put(Context.USER_SERVICE,
                StandIn.of(IUserManager.class, new UserManager()).asBinder());
        services.put(Context.INPUT_METHOD_SERVICE, StandIn.of(IInputMethodManager.class,
                new InputMethodManager()).asBinder());
        return services;
    }

    /**
     * Serves {@code services} in this process: a stand-in servicemanager that knows them
     * replaces {@code ServiceManager.sServiceManager} (without a binder driver
     * BinderInternal.getContextObject() has no servicemanager to return), and they are
     * put in ServiceManager's cache, as bindApplication does on a device.
     */
    public static void installInProcess(Map<String, IBinder> services)
            throws ReflectiveOperationException {
        Map<String, IBinder> all = new TreeMap<>(services);
        StandIn<IServiceManager> sm =
                StandIn.of(IServiceManager.class, new ServiceManagerStandIn(all));
        Field f = ServiceManager.class.getDeclaredField("sServiceManager");
        f.setAccessible(true);
        f.set(null, sm.asInterface());
        ServiceManager.initServiceCache(new TreeMap<>(all));
    }

    /** servicemanager in a single process: the services above, and nothing else. */
    public static final class ServiceManagerStandIn extends IServiceManager.Default {
        private final Map<String, IBinder> mServices;

        ServiceManagerStandIn(Map<String, IBinder> services) {
            mServices = services;
        }

        @Override
        public IBinder getService(String name) {
            return mServices.get(name);
        }

        @Override
        public IBinder checkService(String name) {
            return mServices.get(name);
        }

        @Override
        public Service getService2(String name) {
            ServiceWithMetadata s = new ServiceWithMetadata();
            s.service = mServices.get(name);
            return Service.serviceWithMetadata(s);
        }

        @Override
        public Service checkService2(String name) {
            return getService2(name);
        }

        @Override
        public void addService(String name, IBinder service, boolean allowIsolated,
                int dumpPriority) {
            mServices.put(name, service);
        }

        @Override
        public String[] listServices(int dumpPriority) {
            return mServices.keySet().toArray(new String[0]);
        }

        @Override
        public boolean isDeclared(String name) {
            return false;
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
        public OverlayProperties getOverlaySupport() {
            // DisplayManagerService answers the composer's; the display has none of its
            // own (the natives are stand-ins until the graphics track's).
            return OverlayProperties.getDefault();
        }

        @Override
        public int getPreferredWideGamutColorSpaceId() {
            return 0;  // ColorSpace.Named.SRGB's id: the display has no wide color gamut
        }

        @Override
        public Point getStableDisplaySize() {
            return new Point(DeviceProfile.WIDTH, DeviceProfile.HEIGHT);
        }
    }

    /**
     * input_method: InputMethodManagerService, with no input method. A window's
     * ViewRootImpl needs the service to exist (InputMethodManager's instance); every
     * call answers its default (no IME, nothing shown).
     */
    public static final class InputMethodManager extends IInputMethodManager.Default {
        @Override
        public void addClient(com.android.internal.inputmethod.IInputMethodClient client,
                com.android.internal.inputmethod.IRemoteInputConnection inputConnection,
                int untrustedDisplayId) {
            // No input method will ever bind to it.
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
