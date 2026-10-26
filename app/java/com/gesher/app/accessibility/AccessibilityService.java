/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.accessibility;

import android.content.pm.ParceledListSlice;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.view.IWindow;
import android.view.MagnificationSpec;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityManager;
import android.view.accessibility.IAccessibilityInteractionConnection;
import android.view.accessibility.IAccessibilityManager;
import android.view.accessibility.IAccessibilityManagerClient;

import com.gesher.server.StandIn;

import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * {@code accessibility}: AccessibilityManagerService on Windows (ADR 0058,
 * docs/services.md "Accessibility"), served in every process, since what consumes
 * Android's accessibility on Windows is UI Automation, which reads each app process's
 * windows in that process ({@link UiaBridge}). It is what the process's
 * AccessibilityManager talks to: its clients get the state (enabled while a Windows screen
 * reader runs, a UI Automation client has read one of the process's windows, or
 * $GESHER_ACCESSIBILITY=1), and setState when it changes; ViewRootImpl registers its
 * windows' interaction connections; the views' AccessibilityEvents become UIA events
 * (focus, announcements, invocations, value and structure changes).
 */
public final class AccessibilityService extends IAccessibilityManager.Default {
    static final String TAG = "gesher-a11y";

    private static volatile AccessibilityService sInstance;

    private final List<IAccessibilityManagerClient> mClients = new ArrayList<>();
    /** Window token (IWindow's binder) -> accessibility window id, and back. */
    private final Map<IBinder, Integer> mWindowIds = new HashMap<>();
    private final Map<Integer, IBinder> mWindowTokens = new HashMap<>();
    private int mNextWindowId = 1;

    private volatile boolean mScreenReader;
    private volatile boolean mClientSeen;
    private final boolean mForced = "1".equals(System.getenv("GESHER_ACCESSIBILITY"));
    private int mState;

    private AccessibilityService() {
    }

    static AccessibilityService get() {
        return sInstance;
    }

    static void log(String s) {
        System.out.println(TAG + ": " + s);
        System.out.flush();
    }

    /**
     * Serves {@code accessibility} in this process (ServiceManager's cache, as the camera
     * service) and starts the UI Automation bridge. Where each process sets up its local
     * services (gesher-app's single process, app processes).
     */
    public static void installInProcess() {
        AccessibilityService service = new AccessibilityService();
        try {
            IBinder binder = StandIn.of(IAccessibilityManager.class, service).asBinder();
            Field f = ServiceManager.class.getDeclaredField("sCache");
            f.setAccessible(true);
            @SuppressWarnings("unchecked")
            Map<String, IBinder> cache = (Map<String, IBinder>) f.get(null);
            synchronized (cache) {
                cache.put(android.content.Context.ACCESSIBILITY_SERVICE, binder);
            }
            sInstance = service;
        } catch (ReflectiveOperationException | RuntimeException e) {
            log("no accessibility service in this process: " + e);
            return;
        }
        if (UiaBridge.start()) {
            service.mScreenReader = UiaBridge.screenReader();
        }
        service.update("started");
    }

    /** Windows' screen reader flag changed (WM_SETTINGCHANGE SPI_SETSCREENREADER). */
    void screenReaderChanged() {
        mScreenReader = UiaBridge.screenReader();
        update("screen reader " + (mScreenReader ? "on" : "off"));
    }

    /** A UI Automation client read one of this process's windows. */
    void clientSeen() {
        if (mClientSeen) return;
        mClientSeen = true;
        update("a UI Automation client reads the app's windows");
    }

    private int computeState() {
        return mScreenReader || mClientSeen || mForced
                ? AccessibilityManager.STATE_FLAG_ACCESSIBILITY_ENABLED : 0;
    }

    private void update(String why) {
        List<IAccessibilityManagerClient> clients;
        int state = computeState();
        synchronized (this) {
            if (state == mState && !why.equals("started")) return;
            mState = state;
            clients = new ArrayList<>(mClients);
        }
        if (state != 0 || !why.equals("started")) {
            log("accessibility " + (state != 0 ? "enabled" : "disabled") + " (" + why + "), "
                    + clients.size() + " client(s)");
        }
        for (IAccessibilityManagerClient c : clients) {
            try {
                c.setRelevantEventTypes(state != 0 ? AccessibilityEvent.TYPES_ALL_MASK : 0);
                c.setState(state);
            } catch (RemoteException | RuntimeException e) {
                synchronized (this) {
                    mClients.remove(c);
                }
            }
        }
    }

    boolean enabled() {
        return mState != 0;
    }

    // ---- IAccessibilityManager ----

    @Override
    public long addClient(IAccessibilityManagerClient client, int userId) {
        int state;
        synchronized (this) {
            if (client != null && !mClients.contains(client)) mClients.add(client);
            state = mState;
        }
        int events = state != 0 ? AccessibilityEvent.TYPES_ALL_MASK : 0;
        return ((long) state << 32) | (events & 0xffffffffL);
    }

    @Override
    public boolean removeClient(IAccessibilityManagerClient client, int userId) {
        synchronized (this) {
            return mClients.remove(client);
        }
    }

    @Override
    public void sendAccessibilityEvent(AccessibilityEvent event, int userId) {
        if (event == null) return;
        IBinder token;
        synchronized (this) {
            token = mWindowTokens.get(event.getWindowId());
        }
        UiaBridge.onEvent(event, token);
    }

    @Override
    public int addAccessibilityInteractionConnection(IWindow window, IBinder leashToken,
            IAccessibilityInteractionConnection connection, String packageName, int userId) {
        if (window == null) return -1;
        IBinder token = window.asBinder();
        int id;
        synchronized (this) {
            Integer known = mWindowIds.get(token);
            if (known != null) return known;
            id = mNextWindowId++;
            mWindowIds.put(token, id);
            mWindowTokens.put(id, token);
        }
        log("window " + id + " of " + packageName + " registered");
        return id;
    }

    @Override
    public void removeAccessibilityInteractionConnection(IWindow window) {
        if (window == null) return;
        synchronized (this) {
            Integer id = mWindowIds.remove(window.asBinder());
            if (id != null) mWindowTokens.remove(id);
        }
    }

    @Override
    public int getAccessibilityWindowId(IBinder windowToken) {
        synchronized (this) {
            Integer id = mWindowIds.get(windowToken);
            return id != null ? id : -1;
        }
    }

    @Override
    public IBinder getWindowToken(int windowId, int userId) {
        synchronized (this) {
            return mWindowTokens.get(windowId);
        }
    }

    @Override
    public IAccessibilityManager.WindowTransformationSpec getWindowTransformationSpec(int windowId) {
        // Gesher's windows are shown as they are laid out: no magnification, no transform.
        IAccessibilityManager.WindowTransformationSpec spec =
                new IAccessibilityManager.WindowTransformationSpec();
        spec.transformationMatrix = new float[] {1, 0, 0, 0, 1, 0, 0, 0, 1};
        spec.magnificationSpec = new MagnificationSpec();
        return spec;
    }

    @Override
    public ParceledListSlice<android.accessibilityservice.AccessibilityServiceInfo>
            getInstalledAccessibilityServiceList(int userId) {
        return ParceledListSlice.emptyList();
    }

    @Override
    public List<android.accessibilityservice.AccessibilityServiceInfo>
            getEnabledAccessibilityServiceList(int feedbackType, int userId) {
        return new ArrayList<>();
    }

    @Override
    public long getRecommendedTimeoutMillis() {
        return 0;
    }

    @Override
    public int getFocusStrokeWidth() {
        return 4;
    }

    @Override
    public int getFocusColor() {
        return 0xbf39b500;
    }

    @Override
    public boolean isAccessibilityTargetAllowed(String packageName, int uid, int userId) {
        return true;
    }

    @Override
    public List<String> getAccessibilityShortcutTargets(int shortcutType, int userId) {
        return new ArrayList<>();
    }
}
