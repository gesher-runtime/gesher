/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.view.IWindow;
import android.view.IWindowFocusObserver;
import android.view.IWindowId;
import android.view.WindowId;
import com.gesher.app.window.WindowSession;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.Map;

/** Stable production window identity, as used by AndroidX transitions. No native UI. */
public final class WindowIdTest {
    private static int checks;
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    @SuppressWarnings("unchecked")
    public static void main(String[] args) throws Exception {
        Constructor<?> constructor = WindowSession.class.getDeclaredConstructor(boolean.class);
        constructor.setAccessible(true);
        WindowSession session = (WindowSession) constructor.newInstance(false);
        Class<?> windowType = Class.forName(WindowSession.class.getName() + "$AppWindow");
        Constructor<?> windowConstructor = windowType.getDeclaredConstructor(IWindow.class);
        windowConstructor.setAccessible(true);
        IBinder first = new Binder(), second = new Binder();
        IWindow client = new IWindow.Default() { @Override public IBinder asBinder() { return first; } };
        Object window = windowConstructor.newInstance(client);
        Field windows = WindowSession.class.getDeclaredField("mWindows"); windows.setAccessible(true);
        Map<IBinder,Object> records = (Map<IBinder,Object>) windows.get(session);
        records.put(first, window);
        IWindow secondClient = new IWindow.Default() { @Override public IBinder asBinder() { return second; } };
        records.put(second, windowConstructor.newInstance(secondClient));
        IWindowId identity = session.getWindowId(first);
        require(identity != null, "Attached window has no WindowId");
        require(identity.asBinder().equals(session.getWindowId(first).asBinder()), "Window identity changed between lookups");
        WindowId wrapped = new WindowId(identity);
        require(wrapped.equals(new WindowId(session.getWindowId(first)))
                && wrapped.hashCode() == new WindowId(identity).hashCode(), "Framework identity equality/hashCode are unstable");
        require(!wrapped.equals(new WindowId(session.getWindowId(second))), "Independent windows share an identity");
        require(session.getWindowId(new Binder()) == null, "Unknown token acquired a WindowId");
        int[] gained = {0}, lost = {0};
        IWindowFocusObserver observer = new IWindowFocusObserver.Stub() {
            @Override public void focusGained(IBinder token) { require(token.equals(identity.asBinder()), "Wrong focus identity"); gained[0]++; }
            @Override public void focusLost(IBinder token) { require(token.equals(identity.asBinder()), "Wrong focus identity"); lost[0]++; }
        };
        identity.registerFocusObserver(observer);
        Method focus = identity.getClass().getDeclaredMethod("focus", boolean.class, Handler.class); focus.setAccessible(true);
        focus.invoke(identity, true, null);
        require(identity.isFocused() && gained[0] == 1, "Focus gain was not reported");
        focus.invoke(identity, true, null);
        require(gained[0] == 1, "Unchanged focus was reported twice");
        focus.invoke(identity, false, null);
        require(!identity.isFocused() && lost[0] == 1, "Focus loss was not reported");
        identity.unregisterFocusObserver(observer);
        focus.invoke(identity, true, null);
        require(gained[0] == 1, "Unregistered observer was called");
        session.remove(first);
        require(!identity.isFocused() && session.getWindowId(first) == null, "Removed window retained focus or lookup authority");
        focus.invoke(identity, true, null);
        require(!identity.isFocused(), "Queued focus revived a removed window");
        records.put(first, windowConstructor.newInstance(client));
        require(!wrapped.equals(new WindowId(session.getWindowId(first))), "Reused client token revived the retired window identity");
        System.out.println("WindowIdTest all passed: " + checks + " checks");
    }
}
