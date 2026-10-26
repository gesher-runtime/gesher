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
import android.view.InsetsSource;
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
import com.gesher.app.input.WindowsInputMethod;
import com.gesher.server.WindowManagerService;

import java.io.FileDescriptor;
import java.lang.ref.WeakReference;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

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
 * <p>Popups (docs/graphics.md, "Dialogs, menus and other popups"; ADR 0040): a window
 * that belongs to another one (a dialog to its Activity's window, a PopupWindow, menu,
 * dropdown or tooltip to the window it was attached to, a toast to the app's active window,
 * an Activity with a dialog or translucent theme to the window of the Activity below it)
 * is a popup Win32 window owned by that window: no taskbar button, laid out in the app
 * window's bounds (its display) by its gravity, x and y, placed there and moving with it,
 * stacked by its Android layer, with its own input channel. The session keeps each app
 * window's group of popups ({@link #updateGroup}): the dim behind a dialog, the order, where
 * input goes (a modal popup gets the touches outside it, the focused window the keys) and,
 * with hidden windows (never activated), which window has focus.
 *
 * <p>An Activity started on top of another in the same task takes the window's place:
 * its window opens where the task's window is, at its size, and the window below is hidden
 * when its Activity stops, so the task looks like one window (ADR 0040).
 *
 * <p>Closing a Win32 window (its close button, Alt+F4, WM_CLOSE) goes to the close
 * handler ({@link #setCloseHandler}); by default an Activity's window finishes its
 * Activity. Closing a popup that is not an Activity's window presses Back in it. The
 * session runs in the app's process, where the windows are drawn; it is an
 * {@code IWindowSession.Stub}, so WindowManagerService's session factory can hand it out.
 */
public final class WindowSession extends IWindowSession.Stub {
    private static final String TAG = "gesher-app";
    private static final ExecutorService sDebugCapture = Executors.newSingleThreadExecutor(r -> {
        Thread thread = new Thread(r, "window-debug-capture");
        thread.setDaemon(true);
        return thread;
    });

    /** What closing a window does. Runs on the thread that added the window. */
    public interface CloseHandler {
        /** {@code attrs}: the window's layout params ({@code attrs.token}: its Activity's). */
        void onClose(WindowSession session, IBinder window, WindowManager.LayoutParams attrs);
    }

    /**
     * What the user does to an Activity's top-level window that its lifecycle follows
     * (docs/lifecycle.md). Runs on the thread that added the window. The default tells the
     * system server ({@link #serverLifecycle}); gesher-app's single process drives its one
     * Activity itself (WindowMode.show).
     */
    public interface LifecycleHandler {
        /**
         * The window was minimized ({@code visible} false: its task goes to the back, its
         * Activities stop) or restored (true: the task comes to the front, its top Activity
         * resumes). {@code token}: the Activity's; {@code taskId}: its task, or -1.
         */
        void onTaskVisibility(IBinder token, int taskId, boolean visible);

        /** The user activated the window (shown windows): its task comes to the front. */
        void onTaskActivated(IBinder token, int taskId);

        /**
         * The user resized the window: its task is {@code bounds} (pixels, from 0,0) in size
         * now ({@link TaskConfiguration}). The window's own configuration already followed.
         */
        default void onTaskResized(IBinder token, int taskId, Rect bounds) {
        }
    }

    private static WindowSession sInstance;

    private final boolean mShow;
    private final WindowLayout mLayout = new WindowLayout();
    private final ArrayMap<IBinder, AppWindow> mWindows = new ArrayMap<>();
    private volatile CloseHandler mCloseHandler = WindowSession::finishActivity;
    private volatile LifecycleHandler mLifecycleHandler = serverLifecycle();
    /** Windows added so far (AppWindow.seq). Guarded by mWindows. */
    private long mNextSeq;
    /** The top-level window shown or focused last: a toast's owner. Guarded by mWindows. */
    private AppWindow mActiveRoot;
    /** Where each task's window was last (client area on the screen). Guarded by mWindows. */
    private final android.util.SparseArray<Rect> mTaskScreen = new android.util.SparseArray<>();

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

        // Popups (ADR 0040).
        /** The window this one belongs to (shown as its popup), or null: a top-level window. */
        AppWindow parent;
        /** Shown as a popup Win32 window (owned by its parent's, if it has one). */
        boolean popup;
        /** When it was added: windows of a layer stack in this order. */
        long seq;
        /** Its last frame, in its display's (the app window's client area) coordinates. */
        final Rect frame = new Rect();
        /** Where its popup Win32 window was put (frame and anchor), to skip repeats. */
        final Rect placed = new Rect();
        /** Its Activity's task, or -1. */
        int taskId = -1;
        /** The app hid it, but it stays shown under an Activity's translucent popup. */
        boolean appHidden;
        /** The focus last given to ViewRootImpl by the session (hidden windows). */
        Boolean sessionFocus;
        /** The size ViewRootImpl last asked for (relayout), for laying it out again. */
        int requestedWidth = WindowLayout.UNSPECIFIED_LENGTH;
        int requestedHeight = WindowLayout.UNSPECIFIED_LENGTH;
        /** What updateGroup last logged of the group of this top-level window. */
        String groupState;
        /** A top-level window the user minimized (WM_SIZE SIZE_MINIMIZED), until restored. */
        boolean minimized;
        /** SkiaGL: the dim behind its popups, a layered window of its own (setDimLayer). */
        NativeWindow dimLayer;
        /** What dimLayer was drawn with: alpha, width, height. */
        int dimDrawn;
        int dimWidth;
        int dimHeight;

        // windows3 track (docs/windows3.md).
        /**
         * The display rotation its task is turned to (Surface.ROTATION_*): its Activity's
         * configuration says (the system server's task, for the orientation it asks for).
         */
        int rotation;
        /** The bounds and rotation its Activity's configuration gave last (syncTaskShape). */
        final Rect configBounds = new Rect();
        int configRotation;
        /** A top-level window: where the touch keyboard covers its client area (IME insets). */
        final Rect ime = new Rect();

        AppWindow(IWindow client) {
            this.client = client;
        }

        boolean flag(int f) {
            return (attrs.flags & f) != 0;
        }

        boolean focusable() {
            return !flag(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
        }

        boolean touchable() {
            return !flag(WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE);
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

    public void setLifecycleHandler(LifecycleHandler handler) {
        mLifecycleHandler = handler != null ? handler : serverLifecycle();
    }

    /**
     * The default lifecycle handler: the system server moves the Activity's task, as a
     * desktop's window manager has Android's do (ActivityClient.moveActivityTaskToBack,
     * IActivityTaskManager.moveTaskToFront).
     */
    public static LifecycleHandler serverLifecycle() {
        return new LifecycleHandler() {
            @Override
            public void onTaskVisibility(IBinder token, int taskId, boolean visible) {
                try {
                    if (!visible) {
                        android.app.ActivityClient.getInstance().moveActivityTaskToBack(token,
                                true /* nonRoot */);
                    } else if (taskId >= 0) {
                        moveTaskToFront(taskId);
                    }
                } catch (RuntimeException e) {
                    log("task " + taskId + (visible ? " to the front" : " to the back") + ": "
                            + e);
                }
            }

            @Override
            public void onTaskActivated(IBinder token, int taskId) {
                if (taskId < 0) return;
                try {
                    moveTaskToFront(taskId);
                } catch (RuntimeException e) {
                    log("task " + taskId + " to the front: " + e);
                }
            }

            @Override
            public void onTaskResized(IBinder token, int taskId, Rect bounds) {
                if (taskId < 0) return;
                try {
                    android.app.ActivityTaskManager.getService().resizeTask(taskId, bounds,
                            android.app.ActivityTaskManager.RESIZE_MODE_USER);
                } catch (RemoteException e) {
                    log("task " + taskId + " resized: " + e);
                } catch (RuntimeException e) {
                    log("task " + taskId + " resized: " + e);
                }
            }
        };
    }

    private static void moveTaskToFront(int taskId) {
        ActivityThread thread = ActivityThread.currentActivityThread();
        try {
            android.app.ActivityTaskManager.getService().moveTaskToFront(
                    thread != null ? thread.getApplicationThread() : null,
                    ActivityThread.currentPackageName(), taskId, 0, null);
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
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

    /**
     * The Win32 window (HWND) of the app window whose IWindow binder is {@code windowToken}
     * (View.getWindowToken() of a view attached to it), or 0: where a WebView's WebView2
     * child window goes (webview track, docs/webview.md).
     */
    public long hwndOf(IBinder windowToken) {
        if (windowToken == null) return 0;
        synchronized (mWindows) {
            AppWindow w = mWindows.get(windowToken);
            NativeWindow nw = w != null ? w.nativeWindow : null;
            try {
                return nw != null ? nw.getHwnd() : 0;
            } catch (IllegalStateException closed) {
                return 0;
            }
        }
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
        // A popup lays out in its app window's bounds (its display), as a dialog does in its
        // task's on a device.
        AppWindow root = root(w);
        Rect bounds = root.bounds.isEmpty() ? displayBounds() : new Rect(root.bounds);
        InsetsState state = out.insetsState;
        state.set(new InsetsState(), true /* copySources */);
        state.setDisplayFrame(bounds);
        if (!root.ime.isEmpty()) {
            // The touch keyboard over the app window (windows3 track): the IME's insets, as
            // the IME window's are on a device. adjustResize and adjustPan follow
            // (ViewRootImpl, DecorView).
            InsetsSource ime = new InsetsSource(InsetsSource.ID_IME, WindowInsets.Type.ime());
            ime.setFrame(root.ime);
            ime.setVisible(true);
            state.addSource(ime);
        }
        Rect cutoutSafe = new Rect();
        state.getDisplayCutoutSafe(cutoutSafe);
        ClientWindowFrames frames = out.frames;
        // A sub-window (a PopupWindow, a panel) lays out in its parent's frame, unless it
        // asks for the screen (FLAG_LAYOUT_IN_SCREEN): WindowState's attached frame.
        frames.attachedFrame = isSubWindow(w.attrs.type) && w.parent != null
                ? new Rect(w.parent.frame) : null;
        mLayout.computeFrames(w.attrs, state, cutoutSafe, bounds,
                WindowConfiguration.WINDOWING_MODE_FULLSCREEN, requestedWidth, requestedHeight,
                w.requestedVisibleTypes, 1f /* compatScale */, frames);
        Configuration global = processConfiguration();
        Configuration override = new Configuration();
        if (!root.bounds.isEmpty() || root.rotation != android.view.Surface.ROTATION_0) {
            // Resized by the user: the configuration of a freeform task of the window's size
            // (TaskConfiguration, docs/lifecycle.md): its bounds (ViewRootImpl computes its
            // frames from them, relayoutWindow's local layout), its screen size in dp, smallest
            // width, orientation and screen layout. An Activity that handles those changes gets
            // onConfigurationChanged (ViewRootImpl's activity config callback); the system
            // server relaunches one that does not (resizeTask). A task turned for its
            // Activity's orientation (windows3 track) has the display's rotation too.
            override = TaskConfiguration.override(bounds, root.rotation);
        }
        out.mergedConfiguration.setConfiguration(global, override);
        out.syncSeqId = 0;
    }

    // ---- the Win32 window ----

    /** Creates or resizes the window's Win32 window; true when it was created. */
    private boolean ensureNativeWindow(AppWindow w, int width, int height) {
        if (w.nativeWindow != null) {
            // A window the user resizes keeps the user's size: a relayout computed for an
            // earlier size is followed by the resize still queued (applyResize).
            if (!w.resizable && !w.popup && (w.nativeWindow.getWidth() != width
                    || w.nativeWindow.getHeight() != height)) {
                w.nativeWindow.setClientSize(width, height);
            }
            return false;
        }
        if (w.popup) {
            createPopup(w, width, height);
            return true;
        }
        NativeWindow nw = NativeWindow.create(title(w), width, height);
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
                + (min != null ? ", resizable, at least " + min[0] + "x" + min[1] : "")
                + (w.taskId >= 0 ? ", task " + w.taskId : ""));
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
        // The Win32 window is the size of the frame (a popup's: of its surface, around the
        // frame by the surface insets): client pixels are window pixels.
        if (w.popup) {
            o.offsetX = -w.attrs.surfaceInsets.left;
            o.offsetY = -w.attrs.surfaceInsets.top;
        }
        // Focusable windows start focused (a window shown off the desktop is never
        // activated); a toast, a tooltip or a PopupWindow that is not focusable never is.
        o.initialFocus = w.focusable();
        if (o.initialFocus) w.sessionFocus = Boolean.TRUE;
        final IBinder key = w.client.asBinder();
        o.listener = (type, a, b) -> onWindowEvent(key, type, a, b);
        try {
            w.input = WindowInput.attach(w.nativeWindow.getHwnd(), w.inputChannel, o);
        } catch (RuntimeException | LinkageError e) {
            log("window \"" + w.title() + "\": no input (" + e + ")");
            w.input = null;
        }
        // Text for the window's editor: Windows' text input (input track, ADR 0039).
        WindowsInputMethod ime = WindowsInputMethod.get();
        if (ime != null && w.input != null) {
            ime.addWindow(key, w.input, w.handler, w.title(), w.nativeWindow.getHwnd());
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
                    if (a != 0) onActivated(w);
                });
                break;
            case WindowInput.EVENT_CHAR:
                w.handler.post(() -> deliverChar(key, (char) a));
                break;
            case WindowInput.EVENT_VISIBILITY:
                w.handler.post(() -> onVisibility(key, a != 0));
                break;
            case EVENT_INPUT_PANE:
                w.handler.post(() -> onInputPane(key, a, b));
                break;
            default:
                break;
        }
    }

    /**
     * gesher::WindowEvent::kInputPane (compat/hwui/window): the Windows touch keyboard
     * (IFrameworkInputPane) covers rows a to b of the window's client area (a = -1: none).
     */
    private static final int EVENT_INPUT_PANE = 30;

    /**
     * The touch keyboard came up over a top-level window, moved or went (windows3 track):
     * the part of the window it covers is the IME's insets of the window and its popups
     * (layout), given to them as on a device the IME window's are (IWindow.resized with the
     * new insets state); ViewRootImpl and DecorView then resize the content
     * (SOFT_INPUT_ADJUST_RESIZE) or pan it (ADJUST_PAN) as they do for an IME.
     */
    private void onInputPane(IBinder key, int top, int bottom) {
        AppWindow w;
        synchronized (mWindows) {
            w = mWindows.get(key);
        }
        if (w == null || w.nativeWindow == null || w.popup) return;
        Rect ime = top < 0 || bottom <= top ? new Rect()
                : new Rect(0, top, w.nativeWindow.getWidth(), bottom);
        if (ime.equals(w.ime)) return;
        w.ime.set(ime);
        log("window \"" + w.title() + "\": the touch keyboard "
                + (ime.isEmpty() ? "covers nothing (IME insets none)"
                        : "covers rows " + top + "-" + bottom + " (IME insets "
                                + ime.toShortString() + ")"));
        List<AppWindow> group = new ArrayList<>();
        synchronized (mWindows) {
            for (int i = 0; i < mWindows.size(); i++) {
                AppWindow v = mWindows.valueAt(i);
                if (v.nativeWindow != null && root(v) == w) group.add(v);
            }
        }
        group.sort(WindowSession::compareStack);
        for (AppWindow v : group) {
            WindowRelayoutResult result = new WindowRelayoutResult(new ClientWindowFrames(),
                    new android.util.MergedConfiguration(), new InsetsState(),
                    new android.view.InsetsSourceControl.Array());
            layout(v, v.requestedWidth, v.requestedHeight, result);
            if (v.popup) v.frame.set(result.frames.frame);
            try {
                v.client.resized(result, false /* reportDraw */, true /* forceLayout */,
                        Display.DEFAULT_DISPLAY, false /* syncWithBuffers */,
                        false /* dragResizing */);
            } catch (RemoteException e) {
                log("window \"" + v.title() + "\": resized: " + e);
            }
        }
    }

    /**
     * The touch keyboard covers rows {@code top} to {@code bottom} of the window's client area
     * (top -1: none), as if it were up: for windows without WindowInput (the stand-in
     * natives) and tests.
     */
    public void setInputPane(IBinder windowToken, int top, int bottom) {
        AppWindow w;
        synchronized (mWindows) {
            w = mWindows.get(windowToken);
        }
        if (w != null && w.handler != null) {
            w.handler.post(() -> onInputPane(windowToken, top, bottom));
        }
    }

    /**
     * The user minimized ({@code visible} false) or restored a top-level window
     * (docs/lifecycle.md): its Activity's task goes to the back (its Activities stop, as
     * those of an Android task that is not visible) or comes to the front (its top Activity
     * resumes), through the lifecycle handler. While it is minimized, the windows of its
     * group stay as they are when their Activities stop (Windows keeps the taskbar button
     * and hides the owned popups) instead of being hidden.
     */
    private void onVisibility(IBinder key, boolean visible) {
        AppWindow w;
        synchronized (mWindows) {
            w = mWindows.get(key);
        }
        if (w == null || w.nativeWindow == null || w.popup || w.minimized == !visible) return;
        w.minimized = !visible;
        log("window \"" + w.title() + "\" " + (visible ? "restored" : "minimized")
                + (w.taskId >= 0 ? ", task " + w.taskId : ""));
        mLifecycleHandler.onTaskVisibility(w.attrs.token, w.taskId, visible);
    }

    /** The user closed the Win32 window: the close handler (by default, finish its Activity). */
    private void close(IBinder key) {
        AppWindow w;
        synchronized (mWindows) {
            w = mWindows.get(key);
        }
        if (w == null || w.nativeWindow == null) return;
        if (w.popup && w.attrs.type != WindowManager.LayoutParams.TYPE_BASE_APPLICATION
                && w.input != null) {
            // A dialog, a menu: closing it is Back, as on a phone (Dialog cancels,
            // PopupWindow dismisses).
            log("window \"" + w.title() + "\" closed by the user: Back");
            pressBack(w);
            return;
        }
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
     * match the Win32 window) and draws at the new size. The configuration is a task's of
     * that size (TaskConfiguration); the system server is told (resizeTask) and relaunches
     * the task's Activities that do not handle the change (docs/lifecycle.md).
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
        resizePopups(w);
        if (w.dimLayer != null && w.dimDrawn != 0) updateGroup(w);  // the dim layer's size
        if (w.attrs.type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION) {
            mLifecycleHandler.onTaskResized(w.attrs.token, w.taskId, new Rect(w.bounds));
        }
        if (System.getenv("GESHER_WINDOW_DEBUG") != null) {
            // Debugging: where ViewRootImpl is a second later.
            for (int delay : new int[] {1500, 5000}) {
                w.handler.postDelayed(() -> {
                    log("window debug: after the resize " + viewRootState(viewRoot(w)));
                    if (w.nativeWindow == null) return;
                    log("window debug: back buffer " + grid(w.nativeWindow.readBackBuffer())
                            + ", " + w.nativeWindow.getFramesPresented() + " presented");
                    final NativeWindow nw = w.nativeWindow;
                    sDebugCapture.execute(() -> {
                        try {
                            log("window debug: PrintWindow " + grid(nw.capture()));
                        } catch (RuntimeException e) {
                            log("window debug: " + e);
                        }
                    });
                }, delay);
            }
        }
    }

    /** Debugging: {@code [w, h, ARGB...]} as its black pixels and an 8x8 grid of colors. */
    private static String grid(int[] px) {
        if (px == null || px.length < 3) return "none";
        int w = px[0], h = px[1];
        int black = 0;
        for (int i = 2; i < px.length; i++) {
            if ((px[i] & 0xffffff) == 0) black++;
        }
        StringBuilder s = new StringBuilder(w + "x" + h + ", " + black + " black:");
        for (int y = h / 16; h >= 8 && y < h; y += h / 8) {
            s.append(" |");
            for (int x = w / 16; w >= 8 && x < w; x += w / 8) {
                s.append(String.format(" %06x", px[2 + y * w + x] & 0xffffff));
            }
        }
        return s.toString();
    }

    /** Debugging: a few fields of a ViewRootImpl's layout state. */
    private static String viewRootState(ViewRootImpl root) {
        if (root == null) return "no ViewRootImpl";
        StringBuilder s = new StringBuilder();
        for (String name : new String[] {"mWinFrame", "mWidth", "mHeight", "mLayoutRequested",
                "mTraversalScheduled", "mIsInTraversal", "mNumPausedForSync", "mStopped",
                "mAppVisible", "mForceNextWindowRelayout", "mLastPerformTraversalsSkipDrawReason"}) {
            try {
                Field f = ViewRootImpl.class.getDeclaredField(name);
                f.setAccessible(true);
                s.append(name).append('=').append(f.get(root)).append(' ');
            } catch (ReflectiveOperationException | RuntimeException e) {
                s.append(name).append("=? ");
            }
        }
        View v = root.getView();
        if (v != null) s.append("view ").append(v.getWidth()).append('x').append(v.getHeight());
        return s.toString();
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
            if (type == WindowInput.EVENT_VISIBILITY) onVisibility(key, mEvent[3] != 0);
            if (type == EVENT_INPUT_PANE) onInputPane(key, (int) mEvent[3], (int) mEvent[7]);
        }
        if (close) close(key);
    }

    /**
     * The dim behind a dialog with SkiaGL (ADR 0048): ANGLE's swap chain presents the app
     * window, so it cannot dim what it shows in its paint as the CPU pipeline's frames are
     * dimmed (Window::SetDim). The dim is then what Android's is, a layer of its own: a
     * layered popup window over the app window's client area, black at the dim amount,
     * clicked through, below its popups (they are raised after it).
     */
    private void setDimLayer(AppWindow root, float dim) {
        int alpha = Math.round(Math.max(0f, Math.min(1f, dim)) * 255f);
        NativeWindow layer = root.dimLayer;
        if (alpha == 0) {
            if (layer != null && root.dimDrawn != 0) {
                layer.hide();
                root.dimDrawn = 0;
            }
            return;
        }
        int width = root.nativeWindow.getWidth();
        int height = root.nativeWindow.getHeight();
        if (layer == null) {
            layer = NativeWindow.createPopup("Gesher dim", width, height, root.nativeWindow);
            layer.setTranslucent(true);
            layer.setNoActivate(true, true);
            root.dimLayer = layer;
        }
        if (root.dimDrawn != alpha || root.dimWidth != width || root.dimHeight != height) {
            layer.setPopupBounds(root.nativeWindow, 0, 0, width, height);
            android.view.Surface surface = layer.getSurface();
            try {
                android.graphics.Canvas c = surface.lockCanvas(null);
                c.drawColor(android.graphics.Color.argb(alpha, 0, 0, 0),
                        android.graphics.PorterDuff.Mode.SRC);
                surface.unlockCanvasAndPost(c);
            } catch (RuntimeException e) {
                log("window \"" + root.title() + "\": dim layer: " + e);
            } finally {
                surface.release();
            }
            root.dimDrawn = alpha;
            root.dimWidth = width;
            root.dimHeight = height;
        }
        layer.showPopup(false);
    }

    private void closeNativeWindow(AppWindow w) {
        WindowsInputMethod ime = WindowsInputMethod.get();
        if (ime != null) ime.removeWindow(w.client.asBinder());
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
        if (w.dimLayer != null) {
            w.dimLayer.hide();
            w.dimLayer.close();
            w.dimLayer = null;
            w.dimDrawn = 0;
        }
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

    // ---- popups and tasks (ADR 0040) ----

    /** The top-level window {@code w} belongs to ({@code w} itself when it is one). */
    private static AppWindow root(AppWindow w) {
        AppWindow r = w;
        for (int i = 0; r.parent != null && i < 32; i++) r = r.parent;
        return r;
    }

    private static boolean isSubWindow(int type) {
        return type >= WindowManager.LayoutParams.FIRST_SUB_WINDOW
                && type <= WindowManager.LayoutParams.LAST_SUB_WINDOW;
    }

    private static boolean isApplicationWindow(int type) {
        return type >= WindowManager.LayoutParams.FIRST_APPLICATION_WINDOW
                && type <= WindowManager.LayoutParams.LAST_APPLICATION_WINDOW;
    }

    /**
     * The window {@code w} belongs to, or null for a top-level window (guarded by mWindows):
     * a sub-window's is the window its token names (a PopupWindow's, a panel's: the IWindow
     * of the window it is attached to); an application window's (a dialog's) is its
     * Activity's window; an Activity's window with a translucent or floating theme belongs
     * to the window of the Activity below it; a system window's (a toast's) is the app's
     * active window.
     */
    private AppWindow findParent(AppWindow w, boolean overActivity) {
        int type = w.attrs.type;
        if (isSubWindow(type)) {
            return w.attrs.token != null ? mWindows.get(w.attrs.token) : null;
        }
        if (type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION) {
            if (!overActivity) return null;
            AppWindow below = null;
            for (int i = 0; i < mWindows.size(); i++) {
                AppWindow v = mWindows.valueAt(i);
                if (v != w && v.shown && v.attrs.type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION
                        && (w.taskId < 0 || v.taskId == w.taskId)
                        && (below == null || v.seq > below.seq)) {
                    below = v;
                }
            }
            return below != null ? below : activeRoot();
        }
        if (isApplicationWindow(type)) {
            AppWindow activity = null;
            for (int i = 0; i < mWindows.size(); i++) {
                AppWindow v = mWindows.valueAt(i);
                if (v != w && v.attrs.token == w.attrs.token && w.attrs.token != null
                        && v.attrs.type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION
                        && (activity == null || v.seq > activity.seq)) {
                    activity = v;
                }
            }
            return activity != null ? activity : activeRoot();
        }
        return activeRoot();
    }

    /** The top-level window shown or focused last, or null (guarded by mWindows). */
    private AppWindow activeRoot() {
        if (mActiveRoot != null && mActiveRoot.shown && mActiveRoot.nativeWindow != null
                && mWindows.containsKey(mActiveRoot.client.asBinder())) {
            return mActiveRoot;
        }
        AppWindow best = null;
        for (int i = 0; i < mWindows.size(); i++) {
            AppWindow v = mWindows.valueAt(i);
            if (!v.popup && v.shown && v.nativeWindow != null && (best == null || v.seq > best.seq)) {
                best = v;
            }
        }
        return best;
    }

    /** The Activity whose token is {@code token} in this process, or null. */
    private static android.app.Activity activity(IBinder token) {
        if (token == null) return null;
        ActivityThread thread = ActivityThread.currentActivityThread();
        android.app.Activity a = thread != null ? thread.getActivity(token) : null;
        if (a == null) {
            // gesher-app's single process: its driver's Activity is not ActivityThread's.
            android.app.Activity local = android.app.GesherAppDriver.currentActivity();
            if (local != null && local.getActivityToken() == token) a = local;
        }
        return a;
    }

    /** The task of the Activity whose token is {@code token}, or -1. */
    private static int taskOf(IBinder token) {
        try {
            android.app.Activity a = activity(token);
            return a != null ? a.getTaskId() : -1;
        } catch (RuntimeException e) {
            return -1;
        }
    }

    /**
     * Whether the Activity's theme is translucent or floating (windowIsTranslucent,
     * windowIsFloating, windowSwipeToDismiss: ActivityInfo.isTranslucentOrFloating, what
     * ActivityRecord.occludesParent asks): the Activity below it stays visible.
     */
    private static boolean isTranslucentOrFloating(IBinder token) {
        try {
            android.app.Activity a = activity(token);
            if (a == null || a.getWindow() == null) return false;
            return android.content.pm.ActivityInfo.isTranslucentOrFloating(
                    a.getWindow().getWindowStyle());
        } catch (RuntimeException e) {
            return false;
        }
    }

    private static Field sTaskDescription;

    /**
     * The title of a top-level window: its Activity's TaskDescription label (what Recents and
     * a desktop's caption show for the task) when it set one, else the window's title (its
     * Activity's label).
     */
    private static String title(AppWindow w) {
        if (!w.popup) {
            try {
                android.app.Activity a = activity(w.attrs.token);
                if (a != null) {
                    if (sTaskDescription == null) {
                        Field f = android.app.Activity.class.getDeclaredField("mTaskDescription");
                        f.setAccessible(true);
                        sTaskDescription = f;
                    }
                    android.app.ActivityManager.TaskDescription td =
                            (android.app.ActivityManager.TaskDescription) sTaskDescription.get(a);
                    String label = td != null ? td.getLabel() : null;
                    if (label != null && !label.isEmpty()) return label;
                }
            } catch (ReflectiveOperationException | RuntimeException e) {
                // The window's title.
            }
        }
        return w.title();
    }

    /**
     * The bounds an Activity's window opening in {@code w}'s task takes (guarded by
     * mWindows): those of the task's window shown now, else where it was last; null for the
     * display's.
     */
    private Rect taskBounds(AppWindow w) {
        if (w.taskId < 0) return null;
        for (int i = 0; i < mWindows.size(); i++) {
            AppWindow v = mWindows.valueAt(i);
            if (v != w && !v.popup && v.taskId == w.taskId && v.shown && v.resizable) {
                // A window turned for its Activity's orientation has the turned display's
                // size, not one the user gave the task (windows3 track).
                return v.bounds.isEmpty() || v.rotation != android.view.Surface.ROTATION_0
                        ? null : new Rect(v.bounds);
            }
        }
        Rect last = mTaskScreen.get(w.taskId);
        if (last == null) return null;
        Rect display = displayBounds();
        Rect turned = TaskConfiguration.display(android.view.Surface.ROTATION_90);
        if (last.width() == turned.width() && last.height() == turned.height()) return null;
        return last.width() == display.width() && last.height() == display.height() ? null
                : new Rect(0, 0, last.width(), last.height());
    }

    /** The configuration of the Activity whose token is {@code token} (its resources'), or null. */
    private static Configuration activityConfiguration(IBinder token) {
        try {
            android.app.Activity a = activity(token);
            return a != null ? a.getResources().getConfiguration() : null;
        } catch (RuntimeException e) {
            return null;
        }
    }

    private static int displayRotationOf(Configuration c) {
        int r = c.windowConfiguration.getDisplayRotation();
        return r == WindowConfiguration.ROTATION_UNDEFINED ? android.view.Surface.ROTATION_0 : r;
    }

    /**
     * Whether an Activity's configuration gives its window a shape of its task's (windows3
     * track): a display rotation (the task turned for the orientation the Activity asks for),
     * or bounds that are not the display's (a task the user resized).
     */
    private static boolean hasTaskShape(Configuration c) {
        if (c == null) return false;
        if (displayRotationOf(c) != android.view.Surface.ROTATION_0) return true;
        Rect b = c.windowConfiguration.getBounds();
        return !b.isEmpty() && !b.equals(displayBounds());
    }

    /**
     * An Activity's window follows its task's shape as its configuration gives it (windows3
     * track, docs/windows3.md): when the system server (or the single process) turned the task
     * for the orientation the Activity asks for, its Win32 window takes the turned display's
     * size and its configuration the display's rotation; a configuration that follows a size
     * the user gave the window (resizeTask) changes nothing more, nor one that comes after the
     * user resized it again. Before a relayout, on the window's thread.
     */
    private void syncTaskShape(AppWindow w) {
        if (w.popup || w.attrs.type != WindowManager.LayoutParams.TYPE_BASE_APPLICATION
                || !w.resizable || w.nativeWindow == null) {
            return;
        }
        Configuration c = activityConfiguration(w.attrs.token);
        if (c == null) return;
        Rect b = new Rect(c.windowConfiguration.getBounds());
        if (b.isEmpty()) b = displayBounds();
        int rotation = displayRotationOf(c);
        Rect last = w.configBounds.isEmpty() ? displayBounds() : w.configBounds;
        if (b.equals(last) && rotation == w.configRotation) return;
        boolean turned = rotation != w.configRotation;
        Rect current = w.bounds.isEmpty() ? displayBounds() : w.bounds;
        boolean following = current.equals(last);  // not resized by the user since
        w.configBounds.set(b);
        w.configRotation = rotation;
        if (!turned && !following) return;
        w.rotation = rotation;
        if (b.equals(displayBounds())) {
            w.bounds.setEmpty();
        } else {
            w.bounds.set(0, 0, b.width(), b.height());
        }
        log("window \"" + w.title() + "\" takes its task's shape: " + b.width() + "x" + b.height()
                + ", display " + TaskConfiguration.rotationName(rotation));
        if (w.nativeWindow.getWidth() != b.width() || w.nativeWindow.getHeight() != b.height()) {
            // WM_SIZE then finds the bounds already set (applyResize: no user resize).
            w.nativeWindow.setClientSize(b.width(), b.height());
            resizePopups(w);
        }
    }

    /**
     * An app's own Activity.moveTaskToBack (windows3 track): its task's windows are minimized,
     * not hidden: they keep their taskbar buttons, and restoring one brings the task back
     * (onVisibility: moveTaskToFront, or in a single process the Activity restarted).
     * Minimizing reports itself as the user's minimizing does (WM_SIZE SIZE_MINIMIZED), which
     * takes the task to the back where it is not there yet (the single process). Returns
     * whether a window was minimized.
     */
    public boolean minimizeTask(IBinder activityToken) {
        List<AppWindow> windows = new ArrayList<>();
        synchronized (mWindows) {
            int taskId = -2;
            for (int i = 0; i < mWindows.size(); i++) {
                AppWindow v = mWindows.valueAt(i);
                if (!v.popup && v.attrs.token == activityToken) taskId = v.taskId;
            }
            for (int i = 0; i < mWindows.size(); i++) {
                AppWindow v = mWindows.valueAt(i);
                if (v.popup || v.nativeWindow == null || v.minimized) continue;
                if (v.attrs.token == activityToken || (taskId >= 0 && v.taskId == taskId)) {
                    windows.add(v);
                }
            }
        }
        for (AppWindow v : windows) {
            log("window \"" + v.title() + "\" minimized: its Activity moved its task to the back"
                    + (v.taskId >= 0 ? " (task " + v.taskId + ")" : ""));
            // Minimized from now on (its Activities stopping keep it so, relayout), before
            // WM_SIZE says it (onVisibility then has nothing to do).
            v.minimized = true;
            v.nativeWindow.minimize();
        }
        if (!windows.isEmpty()) {
            // The task goes to the back as when the user minimizes it: the system server has
            // it there already (a second request changes nothing); the single process stops
            // its Activity.
            AppWindow v = windows.get(0);
            Runnable back = () -> mLifecycleHandler.onTaskVisibility(v.attrs.token, v.taskId, false);
            if (v.handler != null) {
                v.handler.post(back);
            } else {
                back.run();
            }
        }
        return !windows.isEmpty();
    }

    /** Remembers where a top-level window of a task is, as it goes or is hidden. */
    private void rememberTaskScreen(AppWindow w) {
        if (w.taskId < 0 || w.nativeWindow == null) return;
        int[] b = w.nativeWindow.getScreenBounds();
        if (b == null) return;
        synchronized (mWindows) {
            mTaskScreen.put(w.taskId, new Rect(b[0], b[1], b[2], b[3]));
        }
    }

    /**
     * A top-level window being shown takes its task's place (ADR 0040): where the task's
     * window is now (an Activity started on top of another) or was last (the Activity below
     * shown again), and its size if the user resized it.
     */
    private void placeInTask(AppWindow w) {
        if (w.taskId < 0) return;
        Rect at = null;
        synchronized (mWindows) {
            for (int i = 0; i < mWindows.size(); i++) {
                AppWindow v = mWindows.valueAt(i);
                if (v != w && !v.popup && v.taskId == w.taskId && v.shown
                        && v.nativeWindow != null) {
                    int[] b = v.nativeWindow.getScreenBounds();
                    if (b != null) at = new Rect(b[0], b[1], b[2], b[3]);
                    break;
                }
            }
            if (at == null && mTaskScreen.get(w.taskId) != null) {
                at = new Rect(mTaskScreen.get(w.taskId));
            }
        }
        if (at == null) return;
        // Its size too, unless its configuration gives it its task's shape (the system
        // server's: the size the user gave the task, or the display turned for its
        // orientation; windows3 track), or the task's window was turned for another
        // Activity's orientation.
        boolean shaped = w.rotation != android.view.Surface.ROTATION_0
                || (!w.configBounds.isEmpty() && !w.configBounds.equals(displayBounds()));
        Rect turned = TaskConfiguration.display(android.view.Surface.ROTATION_90);
        boolean otherTurned = !turned.equals(displayBounds()) && at.width() == turned.width()
                && at.height() == turned.height();
        if (w.resizable && !shaped && !otherTurned && (at.width() != w.nativeWindow.getWidth()
                || at.height() != w.nativeWindow.getHeight())) {
            // WM_SIZE: the window lays out at the task's size (applyResize).
            w.nativeWindow.setClientSize(at.width(), at.height());
        }
        if (mShow) w.nativeWindow.setScreenPosition(at.left, at.top);
        log("window \"" + w.title() + "\" takes task " + w.taskId + "'s place "
                + at.toShortString());
    }

    /** Whether an Activity's window floats over {@code w} as its popup (translucent theme). */
    private boolean hasActivityPopup(AppWindow w) {
        synchronized (mWindows) {
            for (int i = 0; i < mWindows.size(); i++) {
                AppWindow v = mWindows.valueAt(i);
                if (v.parent == w && v.shown
                        && v.attrs.type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION) {
                    return true;
                }
            }
        }
        return false;
    }

    /** Whether hwui renders with SkiaGL (ANGLE's swap chain presents, not the CPU path). */
    private static boolean glRenderer() {
        try {
            return "skiagl".equals(com.gesher.app.graphics.GraphicsDebug.renderer());
        } catch (RuntimeException | LinkageError e) {
            return false;
        }
    }

    /**
     * A popup's Win32 window: owned by its parent's (Windows keeps it above it, hides and
     * minimizes it with it; no taskbar button), translucent when its format has alpha (a
     * layered window: a dialog's rounded corners and shadow show what is behind; with
     * SkiaGL its frames are read back into it), never activated when it is not focusable, clicked
     * through when it is not touchable.
     */
    private void createPopup(AppWindow w, int width, int height) {
        AppWindow root = root(w);
        NativeWindow owner = w.parent != null
                ? (w.parent.nativeWindow != null ? w.parent.nativeWindow : root.nativeWindow)
                : null;
        Rect in = w.attrs.surfaceInsets;
        NativeWindow nw = NativeWindow.createPopup(title(w), width + in.left + in.right,
                height + in.top + in.bottom, owner);
        w.nativeWindow = nw;
        w.resizable = false;
        Looper looper = Looper.myLooper() != null ? Looper.myLooper() : Looper.getMainLooper();
        w.handler = new Handler(looper);
        // With SkiaGL too: hwui draws a translucent popup into a pbuffer read back into the
        // layered window (compat/hwui/gl/angle_gl.cc, ADR 0048).
        boolean translucent = android.graphics.PixelFormat.formatHasAlpha(w.attrs.format);
        nw.setTranslucent(translucent);
        nw.setNoActivate(!w.focusable(), !w.touchable());
        attachInput(w);
        log("window \"" + w.title() + "\": Win32 popup 0x" + Long.toHexString(nw.getHwnd())
                + " owned by " + (owner != null ? "\"" + w.parent.title() + "\" 0x"
                        + Long.toHexString(owner.getHwnd()) : "no window")
                + ", type " + w.attrs.type + (translucent ? ", translucent"
                        + (glRenderer() ? " (SkiaGL frames read back)" : "") : ", opaque")
                + (w.focusable() ? ", focusable" : "") + (w.touchable() ? "" : ", not touchable")
                + ", input " + (w.input != null ? "attached" : "none"));
        if (w.input == null) listenForEvents(w);
    }

    /**
     * Where a popup's surface is: its frame and the surface insets around it (room for the
     * shadow of a window with elevation, as a dialog's or a menu's: ViewRootImpl sets them
     * from its Z, the renderer draws the window that far into the surface). The popup Win32
     * window is the surface; its input is offset back to the frame (attachInput).
     */
    private static Rect surfaceRect(AppWindow w) {
        Rect r = new Rect(w.frame);
        Rect in = w.attrs.surfaceInsets;
        r.set(r.left - in.left, r.top - in.top, r.right + in.right, r.bottom + in.bottom);
        return r;
    }

    /** Puts a popup's Win32 window at its surface in its app window's client area. */
    private void place(AppWindow w) {
        if (w.nativeWindow == null || w.frame.isEmpty()) return;
        AppWindow root = root(w);
        NativeWindow anchor = root != w ? root.nativeWindow : null;
        Rect r = surfaceRect(w);
        if (w.placed.equals(r)) return;
        w.nativeWindow.setPopupBounds(anchor, r.left, r.top, r.width(), r.height());
        w.placed.set(r);
    }

    /** Shows a window once drawn (finishDrawing, or relayout making it visible again). */
    private void showWindow(AppWindow w) {
        NativeWindow nw = w.nativeWindow;
        if (w.popup) {
            AppWindow root = root(w);
            if (root != w && root.nativeWindow != null || mShow) {
                // In its app window (off the desktop when that is); never activated when
                // windows are hidden.
                nw.showPopup(mShow && w.focusable());
            } else {
                nw.showOffscreen();
            }
            w.shown = true;
            log("window \"" + w.title() + "\" shown as a popup of \"" + root.title() + "\" at "
                    + w.frame.toShortString() + ", hwnd 0x" + Long.toHexString(nw.getHwnd()));
            updateGroup(root);
            return;
        }
        placeInTask(w);
        if (mShow) {
            nw.show();
        } else {
            nw.showOffscreen();
        }
        w.shown = true;
        synchronized (mWindows) {
            mActiveRoot = w;
        }
        updateGroup(w);
    }

    private void hideWindow(AppWindow w) {
        if (!w.popup) rememberTaskScreen(w);
        w.nativeWindow.hide();
        w.nativeWindow.setInputRoute(null, null, null);
        w.shown = false;
        w.appHidden = false;
        updateGroup(root(w));
    }

    /** Android's layer of a window type (WindowManagerPolicy.getWindowLayerFromTypeLw). */
    private static int baseLayer(int type) {
        if (isApplicationWindow(type)) return 2;
        switch (type) {
            case WindowManager.LayoutParams.TYPE_TOAST:
                return 8;
            case WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY:
                return 13;
            default:
                return 10;
        }
    }

    /** A sub-window's layer relative to its parent (WindowManagerPolicy.getSubWindowLayerFromTypeLw). */
    private static int subLayer(int type) {
        switch (type) {
            case WindowManager.LayoutParams.TYPE_APPLICATION_MEDIA:
                return -2;
            case WindowManager.LayoutParams.TYPE_APPLICATION_MEDIA_OVERLAY:
                return -1;
            case WindowManager.LayoutParams.TYPE_APPLICATION_SUB_PANEL:
                return 2;
            case WindowManager.LayoutParams.TYPE_APPLICATION_ABOVE_SUB_PANEL:
                return 3;
            default:
                return 1;  // PANEL, ATTACHED_DIALOG
        }
    }

    /** Where a window stacks: compared element by element, a parent before its children. */
    private static long[] stackKey(AppWindow w) {
        if (isSubWindow(w.attrs.type) && w.parent != null) {
            long[] p = stackKey(w.parent);
            long[] k = java.util.Arrays.copyOf(p, p.length + 2);
            k[p.length] = subLayer(w.attrs.type);
            k[p.length + 1] = w.seq;
            return k;
        }
        return new long[] {baseLayer(w.attrs.type), w.seq};
    }

    private static int compareStack(AppWindow a, AppWindow b) {
        long[] x = stackKey(a);
        long[] y = stackKey(b);
        for (int i = 0; i < Math.min(x.length, y.length); i++) {
            if (x[i] != y[i]) return Long.compare(x[i], y[i]);
        }
        return Integer.compare(x.length, y.length);
    }

    private final Object mGroupLock = new Object();

    /**
     * An app window and its shown popups, bottom to top (ADR 0040): the dim behind the
     * topmost dialog asking for it (FLAG_DIM_BEHIND, dimAmount) on the app window; the
     * popups stacked by layer; input routes, as InputDispatcher sends input: a modal popup
     * (focusable, touchable, without FLAG_NOT_TOUCH_MODAL) gets the touches on the windows
     * below it, the topmost focusable window the keys, popups watching outside touches
     * (FLAG_WATCH_OUTSIDE_TOUCH) a press where one starts outside them; with hidden windows
     * (never activated), the focus to that focusable window only.
     */
    private void updateGroup(AppWindow root) {
        if (root == null) return;
        synchronized (mGroupLock) {
            List<AppWindow> group = new ArrayList<>();
            synchronized (mWindows) {
                for (int i = 0; i < mWindows.size(); i++) {
                    AppWindow v = mWindows.valueAt(i);
                    if (v.nativeWindow != null && v.shown && root(v) == root) group.add(v);
                }
            }
            group.sort(WindowSession::compareStack);
            float dim = 0f;
            AppWindow modal = null;
            AppWindow keys = null;
            List<AppWindow> watchers = new ArrayList<>();
            for (AppWindow v : group) {
                if (v != root && v.flag(WindowManager.LayoutParams.FLAG_DIM_BEHIND)) {
                    dim = Math.max(dim, v.attrs.dimAmount);
                }
                if (v != root && v.focusable() && v.touchable()
                        && !v.flag(WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL)) {
                    modal = v;
                }
                if (v.focusable()) keys = v;
                if (v != root && v.flag(WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH)) {
                    watchers.add(v);
                }
            }
            if (root.nativeWindow != null) {
                if (glRenderer()) {
                    setDimLayer(root, dim);
                } else {
                    root.nativeWindow.setDim(dim);
                }
            }
            for (AppWindow v : group) {
                if (v != root) v.nativeWindow.raise();
            }
            int modalAt = group.indexOf(modal);
            StringBuilder desc = new StringBuilder();
            for (int i = 0; i < group.size(); i++) {
                AppWindow v = group.get(i);
                AppWindow pointer = modal != null && modalAt > i ? modal : null;
                int targetAt = pointer != null ? modalAt : i;
                List<NativeWindow> outside = new ArrayList<>();
                for (AppWindow p : watchers) {
                    if (group.indexOf(p) > targetAt) outside.add(p.nativeWindow);
                }
                v.nativeWindow.setInputRoute(pointer != null ? pointer.nativeWindow : null,
                        keys != null && keys != v ? keys.nativeWindow : null,
                        outside.toArray(new NativeWindow[0]));
                desc.append(i == 0 ? "" : ", ").append('"').append(v.title()).append('"');
            }
            if (!mShow) {
                // Hidden windows are never activated: the session gives the focus.
                for (AppWindow v : group) {
                    boolean f = v == keys;
                    if (v.input != null && v.focusable()
                            && (v.sessionFocus == null || v.sessionFocus != f)) {
                        v.sessionFocus = f;
                        v.input.inject(WindowInput.EVENT_FOCUS, 0, 0, f ? 1 : 0, 0, 0, 0, 0, 0,
                                0, 0, 0);
                    }
                }
            }
            String state = "windows of \"" + root.title() + "\", bottom to top: " + desc
                    + (dim > 0 ? "; dim " + dim : "")
                    + (modal != null ? "; modal \"" + modal.title() + "\"" : "")
                    + (keys != null ? "; keys to \"" + keys.title() + "\"" : "");
            if (group.size() > 1 || root.groupState != null) {
                if (!state.equals(root.groupState)) log(state);
                root.groupState = group.size() > 1 ? state : null;
            }
        }
    }

    /**
     * A window was activated (its Win32 window got the focus, shown windows): if a popup of
     * its group takes the keys (a modal dialog over it), that one is brought to the front,
     * as clicking an app window behind its dialog does on Windows.
     */
    private void onActivated(AppWindow w) {
        if (!mShow) return;
        AppWindow root = root(w);
        AppWindow keys = null;
        synchronized (mWindows) {
            List<AppWindow> group = new ArrayList<>();
            for (int i = 0; i < mWindows.size(); i++) {
                AppWindow v = mWindows.valueAt(i);
                if (v.nativeWindow != null && v.shown && root(v) == root) group.add(v);
            }
            group.sort(WindowSession::compareStack);
            for (AppWindow v : group) {
                if (v.focusable()) keys = v;
            }
            if (!w.popup) mActiveRoot = w;
        }
        if (keys != null && keys != w && keys.nativeWindow != null) keys.nativeWindow.activate();
        // The user activated a task's window: the task comes to the front (resumed), as a
        // click on a freeform task does on Android's desktop.
        if (root.taskId >= 0 && !root.minimized
                && root.attrs.type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION) {
            mLifecycleHandler.onTaskActivated(root.attrs.token, root.taskId);
        }
    }

    /** Back (KEYCODE_BACK: VK_BROWSER_BACK) in a window, pressed and released. */
    private static void pressBack(AppWindow w) {
        final int vk = 0xA6;  // VK_BROWSER_BACK
        final int scan = 0x16A;  // E0 6A
        w.input.inject(WindowInput.EVENT_KEY_DOWN, 0, 0, 0, vk, scan, 0, 0, 0, 0, 0, 0);
        w.input.inject(WindowInput.EVENT_KEY_UP, 0, 0, 0, vk, scan, 0, 0, 0, 0, 0, 0);
    }

    /**
     * The app window was resized by the user: its popups lay out again in its new bounds
     * (IWindow.resized with the frames for their last requested sizes), as a dialog does
     * when its task's bounds change.
     */
    private void resizePopups(AppWindow root) {
        List<AppWindow> popups = new ArrayList<>();
        synchronized (mWindows) {
            for (int i = 0; i < mWindows.size(); i++) {
                AppWindow v = mWindows.valueAt(i);
                if (v != root && v.popup && v.nativeWindow != null && root(v) == root) {
                    popups.add(v);
                }
            }
        }
        popups.sort(WindowSession::compareStack);  // parents first: children use their frames
        for (AppWindow v : popups) {
            WindowRelayoutResult result = new WindowRelayoutResult(new ClientWindowFrames(),
                    new android.util.MergedConfiguration(), new InsetsState(),
                    new android.view.InsetsSourceControl.Array());
            layout(v, v.requestedWidth, v.requestedHeight, result);
            v.frame.set(result.frames.frame);
            try {
                v.client.resized(result, false /* reportDraw */, true /* forceLayout */,
                        Display.DEFAULT_DISPLAY, false /* syncWithBuffers */,
                        false /* dragResizing */);
            } catch (RemoteException e) {
                log("window \"" + v.title() + "\": resized: " + e);
            }
        }
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
        // After a LaunchActivityItem: the Activity's calls go through the hook again.
        ActivityClientHook.ensure();
        AppWindow w = new AppWindow(window);
        w.attrs.copyFrom(attrs);
        w.requestedVisibleTypes = requestedVisibleTypes;
        // The Activity's task, and whether the window floats over the Activity below it
        // (a dialog or translucent theme): before mWindows is locked (binder calls).
        boolean overActivity = false;
        Configuration activityConfig = null;
        if (attrs.type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION) {
            w.taskId = taskOf(attrs.token);
            overActivity = isTranslucentOrFloating(attrs.token);
            activityConfig = activityConfiguration(attrs.token);
        }
        synchronized (mWindows) {
            if (mWindows.containsKey(window.asBinder())) {
                return WindowManagerGlobal.ADD_DUPLICATE_ADD;
            }
            w.seq = mNextSeq++;
            w.parent = findParent(w, overActivity);
            w.popup = w.parent != null || attrs.type >= WindowManager.LayoutParams.FIRST_SUB_WINDOW;
            if (activityConfig != null) {
                Rect b = activityConfig.windowConfiguration.getBounds();
                w.configBounds.set(b.isEmpty() ? displayBounds() : b);
                w.configRotation = displayRotationOf(activityConfig);
            }
            if (!w.popup && isUserResizable(w.attrs)) {
                if (hasTaskShape(activityConfig)) {
                    // Its task's shape (windows3 track): the display turned for the
                    // orientation it asks for, or the size the user gave the task.
                    Rect b = w.configBounds;
                    w.rotation = w.configRotation;
                    if (!b.equals(displayBounds())) w.bounds.set(0, 0, b.width(), b.height());
                } else {
                    // An Activity on top of another in its task takes the task window's size.
                    Rect task = taskBounds(w);
                    if (task != null) w.bounds.set(task);
                }
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
                + DeviceProfile.describe() + ")"
                + (w.parent != null ? ", a popup of \"" + w.parent.title() + "\""
                        : w.popup ? ", a popup" : "")
                + (overActivity ? ", over the Activity below (translucent or floating)" : "")
                + (!w.bounds.isEmpty() ? ", bounds " + w.bounds.width() + "x" + w.bounds.height()
                        : "")
                + (w.rotation != android.view.Surface.ROTATION_0 ? ", display turned to "
                        + TaskConfiguration.rotationName(w.rotation) : ""));
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
            boolean wasShown = w.shown;
            if (!w.popup && w.nativeWindow != null && w.shown) rememberTaskScreen(w);
            closeNativeWindow(w);
            log("window \"" + w.title() + "\" removed");
            if (w.popup && wasShown) updateGroup(root(w));
            AppWindow below = w.parent;
            if (below != null && below.appHidden && below.nativeWindow != null
                    && !hasActivityPopup(below) && !root(below).minimized) {
                hideWindow(below);  // its Activity stayed stopped
            }
        }
    }

    @Override
    public int relayout(IWindow window, WindowManager.LayoutParams attrs, int requestedWidth,
            int requestedHeight, int viewVisibility, int flags, int seq, int lastSyncSeqId,
            WindowRelayoutResult outRelayoutResult, SurfaceControl outSurface) {
        AppWindow w = window(window);
        if (w == null) return 0;
        int oldFlags = w.attrs.flags;
        if (attrs != null) {
            w.attrs.copyFrom(attrs);
            if (w.nativeWindow != null) w.nativeWindow.setTitle(title(w));
        }
        w.requestedWidth = requestedWidth;
        w.requestedHeight = requestedHeight;
        // The task turned for an orientation (or back): the window takes its new shape.
        syncTaskShape(w);
        ActivityClientHook.ensure();
        WindowRelayoutResult result = outRelayoutResult != null ? outRelayoutResult
                : new WindowRelayoutResult(new ClientWindowFrames(),
                        new android.util.MergedConfiguration(), new InsetsState(),
                        new android.view.InsetsSourceControl.Array());
        layout(w, requestedWidth, requestedHeight, result);
        Rect frame = result.frames.frame;
        int width = frame.width();
        int height = frame.height();
        if (System.getenv("GESHER_WINDOW_DEBUG") != null && w.nativeWindow != null
                && (w.nativeWindow.getWidth() != width || w.nativeWindow.getHeight() != height
                        || !w.bounds.isEmpty())) {
            log("window debug: relayout " + requestedWidth + "x" + requestedHeight + " in "
                    + w.bounds.toShortString() + ": frame " + frame.toShortString());
        }
        int res = 0;
        if (viewVisibility == View.VISIBLE && width > 0 && height > 0) {
            w.frame.set(frame);
            w.appHidden = false;
            if (ensureNativeWindow(w, width, height)) {
                res |= WindowManagerGlobal.RELAYOUT_RES_FIRST_TIME;
            }
            if (w.popup) place(w);
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
                showWindow(w);
            } else if (w.shown && w.popup && oldFlags != w.attrs.flags) {
                updateGroup(root(w));  // focusable, touchable, dim: the group changes
            }
        } else {
            if (outSurface != null) outSurface.release();
            if (w.nativeWindow != null && w.shown) {
                if (root(w).minimized) {
                    // Its Activity stopped as the user minimized its window: the window
                    // stays minimized (its taskbar button), not hidden, until restored.
                    if (!w.appHidden) log("window \"" + w.title() + "\" stays minimized");
                    w.appHidden = true;
                } else if (hasActivityPopup(w)) {
                    // Its Activity stopped under an Activity with a translucent or floating
                    // theme: it stays on the screen (its last frame) as the one below it does
                    // on a device, until that one goes.
                    if (!w.appHidden) {
                        log("window \"" + w.title() + "\" stays shown under an Activity's popup");
                    }
                    w.appHidden = true;
                } else {
                    hideWindow(w);
                }
            }
        }
        return res;
    }

    @Override
    public void relayoutAsync(IWindow window, WindowManager.LayoutParams attrs,
            int requestedWidth, int requestedHeight, int viewVisibility, int flags, int seq,
            int lastSyncSeqId) {
        AppWindow w = window(window);
        if (w == null) return;
        int oldFlags = w.attrs.flags;
        if (attrs != null) {
            w.attrs.copyFrom(attrs);
            if (w.nativeWindow != null) w.nativeWindow.setTitle(title(w));
        }
        // ViewRootImpl laid the window out itself (only its size or only its position
        // changed): the Win32 window follows.
        w.requestedWidth = requestedWidth;
        w.requestedHeight = requestedHeight;
        if (w.nativeWindow == null || viewVisibility != View.VISIBLE) return;
        WindowRelayoutResult result = new WindowRelayoutResult(new ClientWindowFrames(),
                new android.util.MergedConfiguration(), new InsetsState(),
                new android.view.InsetsSourceControl.Array());
        layout(w, requestedWidth, requestedHeight, result);
        Rect frame = result.frames.frame;
        if (frame.width() <= 0 || frame.height() <= 0) return;
        w.frame.set(frame);
        if (w.popup) {
            place(w);
        } else {
            ensureNativeWindow(w, frame.width(), frame.height());
        }
        if (w.shown && w.popup && oldFlags != w.attrs.flags) updateGroup(root(w));
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
                // PrintWindow sends messages to child windows, including WebView2, whose
                // navigation callback can be waiting for this Looper. Keep it off the UI thread.
                final NativeWindow nw = w.nativeWindow;
                new android.os.Handler(Looper.myLooper()).postDelayed(() -> {
                    try {
                        int[] back = nw.readBackBuffer();
                        long frames = nw.getFramesPresented();
                        sDebugCapture.execute(() -> {
                            try {
                                log("window debug: PrintWindow " + sample(nw.capture())
                                        + ", back buffer " + sample(back) + ", " + frames
                                        + " presented");
                            } catch (RuntimeException e) {
                                log("window debug: " + e);
                            }
                        });
                    } catch (RuntimeException e) {
                        log("window debug: " + e);
                    }
                }, 1000);
            }
        }
        if (!w.shown) {
            // Hidden windows are shown off the desktop: PrintWindow reads nothing back from
            // a window that is not shown.
            showWindow(w);
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
        // showSoftInput/hideSoftInputFromWindow (input track, ADR 0039).
        WindowsInputMethod ime = WindowsInputMethod.get();
        if (ime != null && window != null) {
            ime.onRequestedVisibleTypes(window.asBinder(), requestedVisibleTypes);
        }
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
