/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.app.IUiModeManager;
import android.app.IUiModeManagerCallback;
import android.app.UiModeManager;
import android.content.res.Configuration;
import android.os.IBinder;
import android.os.RemoteException;

import com.gesher.app.DeviceProfile;
import com.gesher.server.StandIn;
import com.gesher.server.UiModeManagerService;
import com.gesher.server.WindowsHost;

import java.util.concurrent.atomic.AtomicInteger;

/**
 * Dalvik tests for Track W Item W06: Dark / Light Theme Sync & UiModeManager Contract.
 */
public final class UiModeContractTest {
    private static void require(boolean condition, String message) {
        if (!condition) {
            throw new AssertionError(message);
        }
    }

    private static final class TestCallback extends IUiModeManagerCallback.Stub {
        final AtomicInteger notifications = new AtomicInteger(0);
        final AtomicInteger lastState = new AtomicInteger(-1);
        final AtomicInteger overrideCount = new AtomicInteger(0);

        @Override
        public void notifyContrastChanged(float contrast) {
            notifications.incrementAndGet();
        }

        @Override
        public void notifyForceInvertStateChanged(int state) {
            notifications.incrementAndGet();
            lastState.set(state);
        }

        @Override
        public void notifyForceInvertOverrideStateChanged() {
            overrideCount.incrementAndGet();
        }
    }

    public static void main(String[] args) throws Exception {
        System.out.println("Starting UiModeContractTest...");

        UiModeManagerService.resetForTests();
        WindowsHost.setAppsUseLightThemeHook(null);

        UiModeManagerService serviceImpl = new UiModeManagerService();
        StandIn.setStrict(false);
        Object standIn = StandIn.of(IUiModeManager.class, serviceImpl);
        IBinder binder = (IBinder) standIn.getClass().getMethod("asBinder").invoke(standIn);
        require(binder.getClass().getName().endsWith("IUiModeManagerStub"),
                "Expected generated stub IUiModeManagerStub, got: " + binder.getClass().getName());
        IUiModeManager service = IUiModeManager.Stub.asInterface(binder);

        // 1. Test default night mode state
        System.out.println("1. Testing default night mode state...");
        require(service.getNightMode(0) == UiModeManager.MODE_NIGHT_NO,
                "Default getNightMode should be MODE_NIGHT_NO (1), got: " + service.getNightMode(0));
        require(!serviceImpl.isNightModeActivated(),
                "Default isNightModeActivated should be false");
        require((DeviceProfile.configuration().uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_NO,
                "DeviceProfile.configuration().uiMode night mask should be UI_MODE_NIGHT_NO");
        require((serviceImpl.getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_NO,
                "service.getConfiguration().uiMode night mask should be UI_MODE_NIGHT_NO");
        require(service.getCurrentModeType(0) == Configuration.UI_MODE_TYPE_NORMAL,
                "getCurrentModeType should be UI_MODE_TYPE_NORMAL");
        require(service.getContrast(0) == 0f,
                "getContrast should be 0f");
        require(!service.isUiModeLocked(), "isUiModeLocked should be false");
        require(!service.isNightModeLocked(), "isNightModeLocked should be false");

        // 2. Test addCallback receives notification upon setNightMode(MODE_NIGHT_YES)
        System.out.println("2. Testing addCallback and setNightMode(MODE_NIGHT_YES)...");
        TestCallback callback1 = new TestCallback();
        service.addCallback(callback1, 0);

        service.setNightMode(UiModeManager.MODE_NIGHT_YES);
        require(service.getNightMode(0) == UiModeManager.MODE_NIGHT_YES,
                "getNightMode should be MODE_NIGHT_YES (2), got: " + service.getNightMode(0));
        require(serviceImpl.isNightModeActivated(),
                "isNightModeActivated should be true after setNightMode(MODE_NIGHT_YES)");
        require((DeviceProfile.configuration().uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES,
                "DeviceProfile.configuration().uiMode night mask should be UI_MODE_NIGHT_YES");
        require((serviceImpl.getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES,
                "service.getConfiguration().uiMode night mask should be UI_MODE_NIGHT_YES");
        require(callback1.notifications.get() > 0,
                "callback should have received notification on setNightMode(MODE_NIGHT_YES)");

        // 3. Test unregistering callback stops notifications
        System.out.println("3. Testing removeCallback stops notifications...");
        service.removeCallback(callback1, 0);
        int countAfterRemove = callback1.notifications.get();

        // 4. Test setNightMode(MODE_NIGHT_NO) toggles back to light mode and notifies
        System.out.println("4. Testing setNightMode(MODE_NIGHT_NO) toggles back to light mode...");
        TestCallback callback2 = new TestCallback();
        service.addCallback(callback2, 0);

        service.setNightMode(UiModeManager.MODE_NIGHT_NO);
        require(service.getNightMode(0) == UiModeManager.MODE_NIGHT_NO,
                "getNightMode should be MODE_NIGHT_NO (1), got: " + service.getNightMode(0));
        require(!serviceImpl.isNightModeActivated(),
                "isNightModeActivated should be false after setNightMode(MODE_NIGHT_NO)");
        require((DeviceProfile.configuration().uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_NO,
                "DeviceProfile.configuration().uiMode night mask should be UI_MODE_NIGHT_NO");
        require((serviceImpl.getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_NO,
                "service.getConfiguration().uiMode night mask should be UI_MODE_NIGHT_NO");
        require(callback1.notifications.get() == countAfterRemove,
                "Unregistered callback1 should not have received further notifications");
        require(callback2.notifications.get() > 0,
                "Active callback2 should have received notification on toggle to MODE_NIGHT_NO");

        // 5. Test setNightModeActivated(boolean active)
        System.out.println("5. Testing setNightModeActivated(boolean)...");
        boolean resActive = service.setNightModeActivated(true);
        require(resActive, "setNightModeActivated(true) should return true");
        require(serviceImpl.isNightModeActivated(), "isNightModeActivated should be true");
        require((DeviceProfile.configuration().uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES,
                "uiMode night mask should be YES");

        boolean resDeactive = service.setNightModeActivated(false);
        require(resDeactive, "setNightModeActivated(false) should return true");
        require(!serviceImpl.isNightModeActivated(), "isNightModeActivated should be false");
        require((DeviceProfile.configuration().uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_NO,
                "uiMode night mask should be NO");

        // 6. Test Windows Host theme sync in MODE_NIGHT_AUTO
        System.out.println("6. Testing Windows host theme sync in MODE_NIGHT_AUTO...");
        // Windows theme 0 = dark
        WindowsHost.setAppsUseLightThemeHook(() -> 0);
        service.setNightMode(UiModeManager.MODE_NIGHT_AUTO);
        require(service.getNightMode(0) == UiModeManager.MODE_NIGHT_AUTO,
                "getNightMode should be MODE_NIGHT_AUTO");
        require(serviceImpl.isNightModeActivated(),
                "In MODE_NIGHT_AUTO with dark host theme, night mode should be activated");
        require((DeviceProfile.configuration().uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES,
                "uiMode night mask should be UI_MODE_NIGHT_YES when host is dark");

        // Windows theme 1 = light
        WindowsHost.setAppsUseLightThemeHook(() -> 1);
        serviceImpl.onWindowsEvents(WindowsHost.EVENT_SETTING_CHANGED);
        require(!serviceImpl.isNightModeActivated(),
                "In MODE_NIGHT_AUTO with light host theme, night mode should not be activated");
        require((DeviceProfile.configuration().uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_NO,
                "uiMode night mask should be UI_MODE_NIGHT_NO when host is light");

        // Windows theme flips back to dark via EVENT_SETTING_CHANGED
        WindowsHost.setAppsUseLightThemeHook(() -> 0);
        serviceImpl.onWindowsEvents(WindowsHost.EVENT_SETTING_CHANGED);
        require(serviceImpl.isNightModeActivated(),
                "In MODE_NIGHT_AUTO after EVENT_SETTING_CHANGED to dark, night mode should be activated");
        require((DeviceProfile.configuration().uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES,
                "uiMode night mask should be UI_MODE_NIGHT_YES after EVENT_SETTING_CHANGED");

        // Reset
        WindowsHost.setAppsUseLightThemeHook(null);
        service.setNightMode(UiModeManager.MODE_NIGHT_NO);
        UiModeManagerService.resetForTests();

        System.out.println("UiModeContractTest: ALL ASSERTIONS PASSED!");
    }
}
