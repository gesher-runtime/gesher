/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.window;

import android.app.ActivityThread;
import android.app.WindowConfiguration;
import android.content.ClipData;
import android.content.res.Configuration;
import android.graphics.Rect;
import android.graphics.Region;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.MessageQueue;
import android.os.RemoteCallback;
import android.os.RemoteException;
import android.os.SystemClock;
import android.util.ArrayMap;
import android.view.IWindow;
import android.view.IWindowId;
import android.view.IWindowSession;
import android.view.Display;
import android.view.InputChannel;
import android.view.InsetsState;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.SurfaceControl;
import android.view.View;
import android.view.ViewRootImpl;
import android.view.WindowInsets;
import android.view.WindowLayout;
import android.view.WindowManager;
import android.view.WindowManagerGlobal;
import android.view.WindowRelayoutResult;
import android.view.inputmethod.ImeTracker;
import android.window.ClientWindowFrames;
import android.window.InputTransferToken;
import android.window.OnBackInvokedCallbackInfo;

import com.gesher.app.DeviceProfile;
import com.gesher.app.input.VirtualKeyboard;
import com.gesher.app.input.WindowInput;
import com.gesher.server.WindowManagerService;

import java.io.FileDescriptor;
import java.lang.ref.WeakReference;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.List;

/**
 * The window session of an app process on Windows (docs/graphics.md, "An app's window"):
 * what WindowManagerService's {@code Session} does for ViewRootImpl, with a top-level Win32
 * window ({@link NativeWindow}) per app window instead of a SurfaceFlinger layer.
 *
 * <ul>
 *   <li>{@code addToDisplay*}: the window is known, with its frames, configuration and
 *       insets; it is visible ({@code ADD_FLAG_APP_VISIBLE}) and in touch mode.
 *   <li>{@code relayout}: the frames ({@link WindowLayout}, as ViewRootImpl and the server
 *       compute them, on a display of {@link DeviceProfile}'s size with no system bars),
 *       the configuration (the process's), the insets; while the view is visible, a Win32
 *       window the size of the frame and a SurfaceControl standing for it (a stand-in
 *       handle tagged with the window: ViewRootImpl's BLASTBufferQueue hands out the
 *       window's Surface, compat/hwui/window/window_jni.cc).
 *   <li>{@code finishDrawing}: the first frame is drawn (and presented: hwui's CPU
 *       pipeline queues each frame, Surface.unlockAndPost presents the software path), so
 *       the window is shown, unless windows are hidden ({@link #install}).
 *   <li>{@code remove}: the window closes.
 * </ul>
 *
 * <p>Input (docs/input.md, ADR 0027): {@code addToDisplay} opens the window's input
 * channel pair; once the Win32 window exists, {@link WindowInput} publishes its mouse,
 * wheel, key, touch and pen messages on the session's end as MotionEvents and KeyEvents
 * for ViewRootImpl's WindowInputEventReceiver, with the window focused from the start.
 * The rest of the window's events come through WindowInput's listener: closing (below),
 * resizing by the user (an Activity's full-size window is resizable: the window's bounds
 * become the new client area and ViewRootImpl gets the new frames through
 * {@code IWindow.resized}, so it lays out and draws at the new size; the smallest size is
 * the Activity's {@code <layout android:minWidth/minHeight>}, else Android's default for
 * resizable tasks, 220dp), focus, and characters the key map cannot type (WM_CHAR, other
 * keyboard layouts), which reach the focused view as {@code ACTION_MULTIPLE} KeyEvents.
 *
 * <p>Closing a Win32 window (its close button, Alt+F4, WM_CLOSE) goes to the close
 * handler ({@link #setCloseHandler}); by default an Activity's window finishes its
 * Activity. The session runs in the app's process, where the windows are drawn; it is an
 * {@code IWindowSession.Stub}, so WindowManagerService's session factory can hand it out.
 */
public final class WindowSession extends IWindowSession.Stub {
    private static final String TAG = "gesher-app";

    /** What closing a window does. Runs on the thread that added the window. */
    public interface CloseHandler {
        /** {@code attrs}: the window's layout params ({@code attrs.token}: its Activity's). */
        void onClose(WindowSession session, IBinder window, WindowManager.LayoutParams attrs);
    }

    private static WindowSession sInstance;

    private final boolean mShow;
    private final WindowLayout mLayout = new WindowLayout();
    private final ArrayMap<IBinder, AppWindow> mWindows = new ArrayMap<>();
    private volatile CloseHandler mCloseHandler = WindowSession::finishActivity;

    /** One app window: a ViewRootImpl's, keyed by its IWindow's binder. */
    private static final class AppWindow {
        final IWindow client;
        final WindowManager.LayoutParams attrs = new WindowManager.LayoutParams();
        int requestedVisibleTypes = WindowInsets.Type.defaultVisible();
        NativeWindow nativeWindow;
        SurfaceControl surfaceControl;
        /** The window's end of its input channel pair (the client has the other), or null. */
        InputChannel inputChannel;
        /** Publishes the Win32 window's input on {@link #inputChannel}; null until attached. */
        WindowInput input;
        /** The Looper of the thread that created the window (ViewRootImpl's). */
        Handler handler;
        /** Where the window lays out: the display, or the size the user resized it to. */
        final Rect bounds = new Rect();
        /** The user may resize the Win32 window (a full-size application window). */
        boolean resizable;
        /** The last size WM_SIZE reported, not yet applied (guarded by this). */
        int pendingWidth;
        int pendingHeight;
        boolean resizePending;
        boolean focused;
        /** A WM_CHAR high surrogate waiting for its low half. */
        char highSurrogate;
        MessageQueue queue;
        FileDescriptor eventFd;
        boolean drawn;
        boolean shown;

        AppWindow(IWindow client) {
            this.client = client;
        }

        String title() {
            CharSequence t = attrs.accessibilityTitle;
            if (t == null || t.length() == 0) t = attrs.getTitle();
            return t != null && t.length() > 0 ? t.toString() : "Gesher";
        }
    }

    private WindowSession(boolean show) {
        mShow = show;
    }

    /**
     * Makes this process's windows Win32 windows: the session is
     * WindowManagerGlobal's (ViewRootImpl's) and what the in-process WindowManagerService
     * hands out. {@code show}: windows are shown once drawn (false: they stay hidden, for
     * tests that read them back with PrintWindow).
     */
    public static synchronized WindowSession install(boolean show) {
        if (sInstance != null) return sInstance;
        WindowSession session = new WindowSession(show);
        // WindowManagerService.openSession (in this process) hands it out; with the system
        // services in another process, WindowManagerGlobal's session is set directly, as
        // the windows are drawn here.
        WindowManagerService.setSessionFactory(callback -> session);
        try {
            Field f = WindowManagerGlobal.class.getDeclaredField("sWindowSession");
            f.setAccessible(true);
            synchronized (WindowManagerGlobal.class) {
                f.set(null, session);
            }
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("WindowManagerGlobal.sWindowSession", e);
        }
        sInstance = session;
        return session;
    }

    /** The installed session, or null. */
    public static synchronized WindowSession get() {
        return sInstance;
    }

    public void setCloseHandler(CloseHandler handler) {
        mCloseHandler = handler != null ? handler : WindowSession::finishActivity;
    }

    /** The default close handler: finish the window's Activity, if this process has it. */
    private static void finishActivity(WindowSession session, IBinder window,
            WindowManager.LayoutParams attrs) {
        ActivityThread thread = ActivityThread.currentActivityThread();
        android.app.Activity a = thread != null && attrs.token != null
                ? thread.getActivity(attrs.token) : null;
        if (a != null) {
            a.finish();
        } else {
            log("window \"" + attrs.getTitle() + "\" closed; no Activity to finish");
        }
    }

    /** The windows' Win32 windows, for tests: title to NativeWindow. */
    public List<NativeWindow> nativeWindows() {
        List<NativeWindow> out = new ArrayList<>();
        synchronized (mWindows) {
            for (int i = 0; i < mWindows.size(); i++) {
                NativeWindow w = mWindows.valueAt(i).nativeWindow;
                if (w != null) out.add(w);
            }
        }
        return out;
    }

    private static void log(String message) {
        System.out.println(TAG + ": " + message);
        System.out.flush();
    }

    private AppWindow window(IWindow client) {
        synchronized (mWindows) {
            return client != null ? mWindows.get(client.asBinder()) : null;
        }
    }

    // ---- frames, configuration, insets ----

    private static Rect displayBounds() {
        return new Rect(0, 0, DeviceProfile.WIDTH, DeviceProfile.HEIGHT);
    }

    private static Configuration processConfiguration() {
        ActivityThread thread = ActivityThread.currentActivityThread();
        Configuration c = thread != null ? thread.getConfiguration() : null;
        return c != null ? c : DeviceProfile.configuration();
    }

    /** Fills {@code out} (frames, configuration, insets) for the requested size. */
    private void layout(AppWindow w, int requestedWidth, int requestedHeight,
            WindowRelayoutResult out) {
        Rect bounds = w.bounds.isEmpty() ? displayBounds() : new Rect(w.bounds);
        InsetsState state = out.insetsState;
        state.set(new InsetsState(), true /* copySources */);
        state.setDisplayFrame(bounds);
        Rect cutoutSafe = new Rect();
        state.getDisplayCutoutSafe(cutoutSafe);
        ClientWindowFrames frames = out.frames;
        mLayout.computeFrames(w.attrs, state, cutoutSafe, bounds,
                WindowConfiguration.WINDOWING_MODE_FULLSCREEN, requestedWidth, requestedHeight,
                w.requestedVisibleTypes, 1f /* compatScale */, frames);
        Configuration global = processConfiguration();
        out.mergedConfiguration.setConfiguration(global, new Configuration());
        out.syncSeqId = 0;
    }

    // ---- the Win32 window ----

    /** Creates or resizes the window's Win32 window; true when it was created. */
    private boolean ensureNativeWindow(AppWindow w, int width, int height) {
        if (w.nativeWindow != null) {
            if (w.nativeWindow.getWidth() != width || w.nativeWindow.getHeight() != height) {
                w.nativeWindow.setClientSize(width, height);
            }
            return false;
        }
        NativeWindow nw = NativeWindow.create(w.title(), width, height);
        // An Activity's full-size window follows the user's resizing (the window's bounds,
        // applyResize); other windows' sizes follow the app's layout (relayout).
        w.resizable = isUserResizable(w.attrs);
        nw.setResizable(w.resizable);
        int[] min = w.resizable ? minimumSize(w.attrs) : null;
        if (min != null) nw.setMinClientSize(min[0], min[1]);
        w.nativeWindow = nw;
        Looper looper = Looper.myLooper() != null ? Looper.myLooper() : Looper.getMainLooper();
        w.handler = new Handler(looper);
        String icon = appIcon(w.attrs.packageName);
        boolean iconSet = icon != null && nw.setIcon(icon);
        attachInput(w);
        String id = nw.getAppUserModelId();
        log("window \"" + w.title() + "\": Win32 window 0x" + Long.toHexString(nw.getHwnd())
                + " in process " + android.os.Process.myPid() + ", AppUserModelID "
                + (id != null ? id : "none") + ", icon " + (iconSet ? icon : "none")
                + ", input " + (w.input != null ? "attached" : "none")
                + (min != null ? ", resizable, at least " + min[0] + "x" + min[1] : ""));
        if (w.input == null) {
            // No input channel (the stand-in natives): the session reads the window's
            // events itself, for closing.
            listenForEvents(w);
        }
        return true;
    }

    /**
     * The package's icon as a .ico: its Start menu shortcut's (com.gesher.app.shell.AppShell
     * records it, in the package's data directory under gesher-shell), else null.
     */
    private static String appIcon(String packageName) {
        if (packageName == null) packageName = ActivityThread.currentPackageName();
        if (packageName == null) return null;
        try {
            java.util.Properties shortcut =
                    com.gesher.app.shell.AppShell.shortcutOf(packageName);
            String icon = shortcut != null ? shortcut.getProperty("icon") : null;
            if (icon != null && !icon.isEmpty()) return icon;
            java.io.File ico = new java.io.File(com.gesher.server.Device.dataDir(),
                    "data/" + packageName + "/gesher-shell/icon.ico");
            return ico.isFile() ? com.gesher.server.Zygote.windowsPath(ico.getPath()) : null;
        } catch (Throwable t) {
            return null;
        }
    }

    /**
     * The window's input channel pair, as WindowState.openInputChannel makes it: the client's
     * end goes to ViewRootImpl ({@code outInputChannel}, its WindowInputEventReceiver reads
     * it), this side keeps the other ({@link AppWindow#inputChannel}) to write the window's
     * input into. With the stand-in InputChannel natives (no pair) {@code outInputChannel}
     * stays empty, which they accept.
     */
    private static void openInputChannel(AppWindow w, InputChannel outInputChannel) {
        InputChannel[] pair;
        try {
            pair = InputChannel.openInputChannelPair(w.title());
        } catch (RuntimeException | LinkageError e) {
            pair = null;
        }
        if (pair == null || pair.length != 2 || pair[0] == null || pair[1] == null) return;
        pair[1].copyTo(outInputChannel);  // ownership moves to the client's InputChannel
        w.inputChannel = pair[0];
    }

    /**
     * The window's input (docs/input.md): {@link WindowInput} publishes the Win32 window's
     * mouse, wheel, key, touch and pen messages on {@link AppWindow#inputChannel} for
     * ViewRootImpl's WindowInputEventReceiver, and is from then on the only reader of the
     * window's event queue: closing, resizing, focus and characters come through its
     * listener ({@link #onWindowEvent}, on the connection's thread). The window is focused
     * from the start (a window shown off the desktop is never activated), so ViewRootImpl
     * dispatches keys to the focused view. Closed in {@link #closeNativeWindow}. Without an
     * input channel (the stand-in InputChannel natives) nothing is attached.
     *
     * <p>A NativeActivity's window (native-libs track) attaches the same way:
     * {@code WindowInput.attach(hwnd, serverChannel, options)} with the server end of an
     * {@code InputChannel.openInputChannelPair}, its client end read by an
     * {@code InputEventReceiver} (or an InputQueue) on the app's Looper.
     */
    private void attachInput(AppWindow w) {
        if (w.inputChannel == null) return;
        WindowInput.Options o = new WindowInput.Options();
        // The Win32 window is the size of the frame: client pixels are window pixels.
        o.initialFocus = true;
        final IBinder key = w.client.asBinder();
        o.listener = (type, a, b) -> onWindowEvent(key, type, a, b);
        try {
            w.input = WindowInput.attach(w.nativeWindow.getHwnd(), w.inputChannel, o);
        } catch (RuntimeException | LinkageError e) {
            log("window \"" + w.title() + "\": no input (" + e + ")");
            w.input = null;
        }
    }

    /**
     * A window event that is not input (WindowInput's listener, on the connection's thread):
     * handled on the window's Looper.
     */
    private void onWindowEvent(IBinder key, int type, int a, int b) {
        final AppWindow w;
        synchronized (mWindows) {
            w = mWindows.get(key);
        }
        if (w == null || w.handler == null) return;
        switch (type) {
            case WindowInput.EVENT_CLOSE:
                w.handler.post(() -> close(key));
                break;
            case WindowInput.EVENT_RESIZE:
                synchronized (w) {
                    // While the user drags, only the latest size matters.
                    w.pendingWidth = a;
                    w.pendingHeight = b;
                    if (w.resizePending) return;
                    w.resizePending = true;
                }
                w.handler.post(() -> applyResize(key));
                break;
            case WindowInput.EVENT_FOCUS:
                w.handler.post(() -> {
                    if (w.focused != (a != 0)) {
                        w.focused = a != 0;
                        log("window \"" + w.title() + "\" " + (a != 0 ? "focused" : "unfocused"));
                    }
                });
                break;
            case WindowInput.EVENT_CHAR:
                w.handler.post(() -> deliverChar(key, (char) a));
                break;
            default:
                break;
        }
    }

    /** The user closed the Win32 window: the close handler (by default, finish its Activity). */
    private void close(IBinder key) {
        AppWindow w;
        synchronized (mWindows) {
            w = mWindows.get(key);
        }
        if (w == null || w.nativeWindow == null) return;
        log("window \"" + w.title() + "\" closed by the user");
        mCloseHandler.onClose(this, key, w.attrs);
    }

    /** An application window that fills its bounds: the user may resize it. */
    private static boolean isUserResizable(WindowManager.LayoutParams attrs) {
        return attrs.type >= WindowManager.LayoutParams.FIRST_APPLICATION_WINDOW
                && attrs.type <= WindowManager.LayoutParams.LAST_APPLICATION_WINDOW
                && attrs.width == WindowManager.LayoutParams.MATCH_PARENT
                && attrs.height == WindowManager.LayoutParams.MATCH_PARENT;
    }

    /**
     * The smallest client area of a resizable window, in pixels: its Activity's
     * {@code <layout android:minWidth android:minHeight>} (ActivityInfo.windowLayout), else
     * Android's default minimal size of a resizable task
     * (config_defaultMinimalSizeResizableTask, 220dp).
     */
    private static int[] minimumSize(WindowManager.LayoutParams attrs) {
        int fallback = Math.round(220f * DeviceProfile.DENSITY_DPI / 160f);
        int[] min = {fallback, fallback};
        try {
            ActivityThread thread = ActivityThread.currentActivityThread();
            android.app.Activity a = thread != null && attrs.token != null
                    ? thread.getActivity(attrs.token) : null;
            if (a == null) return min;
            Field f = android.app.Activity.class.getDeclaredField("mActivityInfo");
            f.setAccessible(true);
            android.content.pm.ActivityInfo info = (android.content.pm.ActivityInfo) f.get(a);
            android.content.pm.ActivityInfo.WindowLayout layout =
                    info != null ? info.windowLayout : null;
            if (layout != null) {
                if (layout.minWidth > 0) min[0] = layout.minWidth;
                if (layout.minHeight > 0) min[1] = layout.minHeight;
            }
        } catch (ReflectiveOperationException | RuntimeException e) {
            // The default.
        }
        return min;
    }

    /**
     * The user resized the Win32 window (WM_SIZE): the window's bounds become its client
     * area, and ViewRootImpl gets the frames for them (IWindow.resized, as
     * WindowStateResizeItem gives them on a device), lays out, relayouts (whose frames now
     * match the Win32 window) and draws at the new size. The configuration stays the
     * process's (no Activity relaunch).
     */
    private void applyResize(IBinder key) {
        AppWindow w;
        synchronized (mWindows) {
            w = mWindows.get(key);
        }
        if (w == null || w.nativeWindow == null) return;
        int width;
        int height;
        synchronized (w) {
            w.resizePending = false;
            width = w.pendingWidth;
            height = w.pendingHeight;
        }
        if (!w.resizable || width <= 0 || height <= 0) return;
        Rect current = w.bounds.isEmpty() ? displayBounds() : w.bounds;
        if (current.width() == width && current.height() == height) return;
        w.bounds.set(0, 0, width, height);
        WindowRelayoutResult result = new WindowRelayoutResult(new ClientWindowFrames(),
                new android.util.MergedConfiguration(), new InsetsState(),
                new android.view.InsetsSourceControl.Array());
        layout(w, WindowLayout.UNSPECIFIED_LENGTH, WindowLayout.UNSPECIFIED_LENGTH, result);
        log("window \"" + w.title() + "\" resized by the user to " + width + "x" + height
                + " (frame " + result.frames.frame.toShortString() + ")");
        try {
            w.client.resized(result, false /* reportDraw */, true /* forceLayout */,
                    Display.DEFAULT_DISPLAY, false /* syncWithBuffers */,
                    false /* dragResizing */);
        } catch (RemoteException e) {
            log("window \"" + w.title() + "\": resized: " + e);
        }
    }

    private KeyCharacterMap mKeyMap;

    /**
     * A character the window got (WM_CHAR). Those the key map (Generic.kcm, a US layout)
     * types came as key events already; others (another keyboard layout, dead keys, an
     * IME's or a program's WM_CHAR) go to the window's focused view as an
     * {@code ACTION_MULTIPLE} KeyEvent with the text, which TextView inserts
     * (KeyListener.onKeyOther), as Android delivers text that has no key.
     */
    private void deliverChar(IBinder key, char c) {
        AppWindow w;
        synchronized (mWindows) {
            w = mWindows.get(key);
        }
        if (w == null) return;
        String text;
        if (Character.isHighSurrogate(c)) {
            w.highSurrogate = c;
            return;
        } else if (Character.isLowSurrogate(c) && w.highSurrogate != 0) {
            text = new String(new char[] {w.highSurrogate, c});
        } else {
            text = String.valueOf(c);
        }
        w.highSurrogate = 0;
        if (c < 0x20 || c == 0x7f) return;  // control characters: their keys were key events
        if (text.length() == 1) {
            try {
                if (mKeyMap == null) {
                    mKeyMap = VirtualKeyboard.loadKeyCharacterMap(
                            KeyCharacterMap.VIRTUAL_KEYBOARD);
                }
                if (mKeyMap.getEvents(new char[] {c}) != null) return;
            } catch (RuntimeException | LinkageError e) {
                return;
            }
        }
        ViewRootImpl root = viewRoot(w);
        if (root == null) return;
        root.dispatchInputEvent(new KeyEvent(SystemClock.uptimeMillis(), text,
                KeyCharacterMap.VIRTUAL_KEYBOARD, 0));
    }

    /** The window's ViewRootImpl (its IWindow is ViewRootImpl.W, in this process), or null. */
    private static ViewRootImpl viewRoot(AppWindow w) {
        try {
            Field f = w.client.getClass().getDeclaredField("mViewAncestor");
            f.setAccessible(true);
            Object ref = f.get(w.client);
            return ref instanceof WeakReference
                    ? (ViewRootImpl) ((WeakReference<?>) ref).get() : null;
        } catch (ReflectiveOperationException | ClassCastException e) {
            return null;
        }
    }

    private void listenForEvents(AppWindow w) {
        Looper looper = Looper.myLooper() != null ? Looper.myLooper() : Looper.getMainLooper();
        w.queue = looper.getQueue();
        FileDescriptor fd = new FileDescriptor();
        try {
            // libcore's FileDescriptor.setInt$ (not in the JDK's java.base javac sees).
            FileDescriptor.class.getMethod("setInt$", int.class)
                    .invoke(fd, w.nativeWindow.getEventFd());
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("FileDescriptor.setInt$", e);
        }
        w.eventFd = fd;
        final IBinder key = w.client.asBinder();
        w.queue.addOnFileDescriptorEventListener(fd,
                MessageQueue.OnFileDescriptorEventListener.EVENT_INPUT,
                (descriptor, events) -> {
                    dispatchEvents(key);
                    return MessageQueue.OnFileDescriptorEventListener.EVENT_INPUT;
                });
    }

    private final double[] mEvent = new double[13];

    /** The window's queued events, without WindowInput: only closing is handled. */
    private void dispatchEvents(IBinder key) {
        AppWindow w;
        synchronized (mWindows) {
            w = mWindows.get(key);
        }
        if (w == null || w.nativeWindow == null) return;
        int type;
        boolean close = false;
        while ((type = w.nativeWindow.pollEvent(mEvent)) != 0) {
            if (type == NativeWindow.EVENT_CLOSE) close = true;
        }
        if (close) close(key);
    }

    private void closeNativeWindow(AppWindow w) {
        if (w.input != null) {
            // Before the window and the channel go (joins the connection's thread).
            w.input.close();
            w.input = null;
        }
        if (w.queue != null && w.eventFd != null) {
            w.queue.removeOnFileDescriptorEventListener(w.eventFd);
        }
        w.queue = null;
        w.eventFd = null;
        if (w.nativeWindow != null) {
            // Hidden at once; the HWND goes when the last Surface over it is released too.
            w.nativeWindow.hide();
            w.nativeWindow.close();
            w.nativeWindow = null;
        }
        if (w.surfaceControl != null) {
            w.surfaceControl.release();
            w.surfaceControl = null;
        }
        if (w.inputChannel != null) {
            w.inputChannel.dispose();
            w.inputChannel = null;
        }
        w.shown = false;
    }

    // ---- IWindowSession ----

    @Override
    public int addToDisplay(IWindow window, WindowManager.LayoutParams attrs, int viewVisibility,
            int layerStackId, int requestedVisibleTypes, InputChannel outInputChannel,
            WindowRelayoutResult result) {
        return addToDisplayAsUser(window, attrs, viewVisibility, layerStackId, 0,
                requestedVisibleTypes, outInputChannel, result);
    }

    @Override
    public int addToDisplayAsUser(IWindow window, WindowManager.LayoutParams attrs,
            int viewVisibility, int layerStackId, int userId, int requestedVisibleTypes,
            InputChannel outInputChannel, WindowRelayoutResult result) {
        AppWindow w = new AppWindow(window);
        w.attrs.copyFrom(attrs);
        w.requestedVisibleTypes = requestedVisibleTypes;
        synchronized (mWindows) {
            if (mWindows.containsKey(window.asBinder())) {
                return WindowManagerGlobal.ADD_DUPLICATE_ADD;
            }
            mWindows.put(window.asBinder(), w);
        }
        // The window's input channel pair (input reaches it once the input track's
        // WindowInput is attached: attachInput).
        if (outInputChannel != null) {
            openInputChannel(w, outInputChannel);
        }
        if (result != null) {
            layout(w, WindowLayout.UNSPECIFIED_LENGTH, WindowLayout.UNSPECIFIED_LENGTH, result);
        }
        log("window \"" + w.title() + "\" added (type " + attrs.type + ", display "
                + DeviceProfile.describe() + ")");
        return WindowManagerGlobal.ADD_OKAY | WindowManagerGlobal.ADD_FLAG_APP_VISIBLE
                | WindowManagerGlobal.ADD_FLAG_IN_TOUCH_MODE;
    }

    @Override
    public int addToDisplayWithoutInputChannel(IWindow window, WindowManager.LayoutParams attrs,
            int viewVisibility, int layerStackId, WindowRelayoutResult result) {
        return addToDisplayAsUser(window, attrs, viewVisibility, layerStackId, 0,
                WindowInsets.Type.defaultVisible(), null, result);
    }

    @Override
    public void remove(IBinder clientToken) {
        AppWindow w;
        synchronized (mWindows) {
            w = mWindows.remove(clientToken);
        }
        if (w != null) {
            closeNativeWindow(w);
            log("window \"" + w.title() + "\" removed");
        }
    }

    @Override
    public int relayout(IWindow window, WindowManager.LayoutParams attrs, int requestedWidth,
            int requestedHeight, int viewVisibility, int flags, int seq, int lastSyncSeqId,
            WindowRelayoutResult outRelayoutResult, SurfaceControl outSurface) {
        AppWindow w = window(window);
        if (w == null) return 0;
        if (attrs != null) {
            w.attrs.copyFrom(attrs);
            if (w.nativeWindow != null) w.nativeWindow.setTitle(w.title());
        }
        WindowRelayoutResult result = outRelayoutResult != null ? outRelayoutResult
                : new WindowRelayoutResult(new ClientWindowFrames(),
                        new android.util.MergedConfiguration(), new InsetsState(),
                        new android.view.InsetsSourceControl.Array());
        layout(w, requestedWidth, requestedHeight, result);
        Rect frame = result.frames.frame;
        int width = frame.width();
        int height = frame.height();
        int res = 0;
        if (viewVisibility == View.VISIBLE && width > 0 && height > 0) {
            if (ensureNativeWindow(w, width, height)) {
                res |= WindowManagerGlobal.RELAYOUT_RES_FIRST_TIME;
            }
            if (w.surfaceControl == null) {
                w.surfaceControl = new SurfaceControl.Builder()
                        .setName(w.title())
                        .setBufferSize(width, height)
                        .setFormat(w.attrs.format)
                        .setCallsite("WindowSession.relayout")
                        .build();
                w.nativeWindow.tagSurfaceControl(w.surfaceControl.mNativeObject);
                res |= WindowManagerGlobal.RELAYOUT_RES_SURFACE_CHANGED;
            }
            if (outSurface != null) {
                outSurface.copyFrom(w.surfaceControl, "WindowSession.relayout");
            }
            if (w.drawn && !w.shown) {
                if (mShow) {
                    w.nativeWindow.show();
                } else {
                    w.nativeWindow.showOffscreen();
                }
                w.shown = true;
            }
        } else {
            if (outSurface != null) outSurface.release();
            if (w.nativeWindow != null && w.shown) {
                w.nativeWindow.hide();
                w.shown = false;
            }
        }
        return res;
    }

    @Override
    public void relayoutAsync(IWindow window, WindowManager.LayoutParams attrs,
            int requestedWidth, int requestedHeight, int viewVisibility, int flags, int seq,
            int lastSyncSeqId) {
        AppWindow w = window(window);
        if (w != null && attrs != null) {
            w.attrs.copyFrom(attrs);
            if (w.nativeWindow != null) w.nativeWindow.setTitle(w.title());
        }
    }

    @Override
    public boolean outOfMemory(IWindow window) {
        return false;
    }

    @Override
    public void setInsets(IWindow window, int touchableInsets, Rect contentInsets,
            Rect visibleInsets, Region touchableRegion) {
    }

    @Override
    public void finishDrawing(IWindow window, SurfaceControl.Transaction postDrawTransaction,
            int seqId) {
        AppWindow w = window(window);
        if (w == null || w.nativeWindow == null) return;
        if (!w.drawn) {
            w.drawn = true;
            log("window \"" + w.title() + "\" " + w.nativeWindow.getWidth() + "x"
                    + w.nativeWindow.getHeight() + ": first frame drawn ("
                    + w.nativeWindow.getFramesPresented() + " presented), "
                    + (mShow ? "shown" : "hidden") + ", hwnd 0x"
                    + Long.toHexString(w.nativeWindow.getHwnd()));
            if (System.getenv("GESHER_WINDOW_DEBUG") != null) {
                // Debugging: what the window shows (PrintWindow in this process) and its back
                // buffer, a second later.
                final NativeWindow nw = w.nativeWindow;
                new android.os.Handler(Looper.myLooper()).postDelayed(() -> {
                    try {
                        int[] printed = nw.capture();
                        int[] back = nw.readBackBuffer();
                        log("window debug: PrintWindow " + sample(printed) + ", back buffer "
                                + sample(back) + ", " + nw.getFramesPresented() + " presented");
                    } catch (RuntimeException e) {
                        log("window debug: " + e);
                    }
                }, 1000);
            }
        }
        if (!w.shown) {
            // Hidden windows are shown off the desktop: PrintWindow reads nothing back from
            // a window that is not shown.
            if (mShow) {
                w.nativeWindow.show();
            } else {
                w.nativeWindow.showOffscreen();
            }
            w.shown = true;
        }
    }

    /** {@code [w, h, ARGB...]} as a few ARGB values down the window's first quarter. */
    private static String sample(int[] px) {
        if (px == null || px.length < 3) return "none";
        int w = px[0], h = px[1];
        StringBuilder s = new StringBuilder(w + "x" + h);
        for (int i = 1; i <= 7; i++) {
            int x = w / 4, y = h * i / 8;
            s.append(String.format(" (%d,%d) %08x", x, y, px[2 + y * w + x]));
        }
        return s.toString();
    }

    @Override
    public IBinder performDrag(IWindow window, int flags, SurfaceControl surface,
            int touchSource, int touchDeviceId, int touchPointerId, float touchX, float touchY,
            float thumbCenterX, float thumbCenterY, ClipData data) {
        return null;
    }

    @Override
    public boolean dropForAccessibility(IWindow window, int x, int y) {
        return false;
    }

    @Override
    public void reportDropResult(IWindow window, boolean consumed) {
    }

    @Override
    public void cancelDragAndDrop(IBinder dragToken, boolean skipAnimation) {
    }

    @Override
    public void dragRecipientEntered(IWindow window) {
    }

    @Override
    public void dragRecipientExited(IWindow window) {
    }

    @Override
    public void setWallpaperPosition(IBinder windowToken, float x, float y, float xstep,
            float ystep) {
    }

    @Override
    public void setWallpaperZoomOut(IBinder windowToken, float scale) {
    }

    @Override
    public void setShouldZoomOutWallpaper(IBinder windowToken, boolean shouldZoom) {
    }

    @Override
    public void setWallpaperDisplayOffset(IBinder windowToken, int x, int y) {
    }

    @Override
    public void sendWallpaperCommand(IBinder window, String action, int x, int y, int z,
            Bundle extras) {
    }

    @Override
    public void onRectangleOnScreenRequested(IBinder token, Rect rectangle, int source) {
    }

    @Override
    public IWindowId getWindowId(IBinder window) {
        return null;
    }

    @Override
    public void pokeDrawLock(IBinder window) {
    }

    @Override
    public boolean startMovingTask(IWindow window, float startX, float startY) {
        return false;
    }

    @Override
    public void finishMovingTask(IWindow window) {
    }

    @Override
    public void updateTapExcludeRegion(IWindow window, Region region) {
    }

    @Override
    public void updateRequestedVisibleTypes(IWindow window, int requestedVisibleTypes,
            ImeTracker.Token imeStatsToken) {
        AppWindow w = window(window);
        if (w != null) w.requestedVisibleTypes = requestedVisibleTypes;
    }

    @Override
    public void updateAnimatingTypes(IWindow window, int animatingTypes,
            ImeTracker.Token imeStatsToken) {
    }

    @Override
    public void reportSystemGestureExclusionChanged(IWindow window, List<Rect> exclusionRects) {
    }

    @Override
    public void reportDecorViewGestureInterceptionChanged(IWindow window, boolean intercepted) {
    }

    @Override
    public void reportKeepClearAreasChanged(IWindow window, List<Rect> restricted,
            List<Rect> unrestricted) {
    }

    @Override
    public InputChannel grantInputChannel(int displayId, SurfaceControl surface,
            IBinder clientToken, InputTransferToken hostInputTransferToken, int flags,
            int privateFlags, int inputFeatures, int type, IBinder windowToken,
            InputTransferToken embeddedInputTransferToken, String inputHandleName) {
        return null;
    }

    @Override
    public void updateInputChannel(IBinder channelToken,
            InputTransferToken hostInputTransferToken, int displayId, SurfaceControl surface,
            int flags, int privateFlags, int inputFeatures, Region region) {
    }

    @Override
    public void grantEmbeddedWindowFocus(IWindow window, InputTransferToken inputToken,
            boolean grantFocus) {
    }

    @Override
    public void generateDisplayHash(IWindow window, Rect boundsInWindow, String hashAlgorithm,
            RemoteCallback callback) {
    }

    @Override
    public void setOnBackInvokedCallbackInfo(IWindow window,
            OnBackInvokedCallbackInfo callbackInfo) {
    }

    @Override
    public void clearTouchableRegion(IWindow window) {
    }

    @Override
    public boolean cancelDraw(IWindow window, int seqId) {
        return false;
    }

    @Override
    public boolean moveFocusToAdjacentWindow(IWindow fromWindow, int direction) {
        return false;
    }

    @Override
    public void notifyImeWindowVisibilityChangedFromClient(IWindow window, boolean visible,
            ImeTracker.Token statsToken) {
    }
}
