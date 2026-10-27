/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.accessibility;

import android.graphics.Rect;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.view.View;
import android.view.ViewRootImpl;
import android.view.WindowManagerGlobal;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityInteractionClient;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import android.view.accessibility.AccessibilityWindowInfo;
import android.webkit.WebView;

import com.gesher.app.window.WindowSession;

import java.lang.reflect.Field;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Android's views for Windows' UI Automation (ADR 0058): the Java half of
 * compat/accessibility/uia_bridge.cc, which serves UIA providers for the app's Win32
 * windows. Each UIA element is an AccessibilityNodeInfo of the window's view tree, read
 * through its ViewRootImpl's direct accessibility connection
 * ({@code getDirectAccessibilityConnectionId}: the AccessibilityInteractionController, as
 * an accessibility service reads it, in this process); actions are its
 * performAccessibilityAction. The natives call these methods on UI Automation's threads;
 * the interaction client waits (bounded) for the UI thread.
 *
 * <p>A node is described as {@code id\tparent\tclass\tname\tvalue\thelp\tflags\tl t r
 * b\tchildren\thosted\tautomationId}: the window's root view is id -1; bounds are in the
 * window's client coordinates; a WebView has no children but its WebView2 window
 * ("hosted"), whose own UIA tree UIA shows there.
 */
public final class UiaBridge {
    private UiaBridge() {
    }

    static final long ROOT = -1;

    private static Boolean sLoaded;
    private static final ExecutorService sEvents = Executors.newSingleThreadExecutor(r -> {
        Thread t = new Thread(r, "AccessibilityEvents");
        t.setDaemon(true);
        return t;
    });

    static synchronized boolean load() {
        if (sLoaded == null) {
            try {
                System.loadLibrary("gesher_a11y");
                sLoaded = true;
            } catch (UnsatisfiedLinkError e) {
                AccessibilityService.log("no UI Automation bridge: " + e.getMessage());
                sLoaded = false;
            }
        }
        return sLoaded;
    }

    static boolean start() {
        return load() && nativeStart();
    }

    static boolean screenReader() {
        return load() && nativeScreenReader();
    }

    private static native boolean nativeStart();

    private static native boolean nativeScreenReader();

    private static native boolean nativeClientsListening();

    private static native void nativeRaise(long hwnd, long id, int kind, String text);

    private static void raise(long hwnd, long id, int kind, String text) {
        // UIA may query a provider synchronously as an event is raised. Its
        // Android node lookup needs the UI thread, which must remain free.
        sEvents.execute(() -> nativeRaise(hwnd, id, kind, text));
    }

    // ---- Windows ----

    /** A window's root: its ViewRootImpl and direct connection. */
    private static final class Root {
        final ViewRootImpl viewRoot;
        final View view;
        final int connectionId;
        final long rootNodeId;

        Root(ViewRootImpl viewRoot, View view, int connectionId, long rootNodeId) {
            this.viewRoot = viewRoot;
            this.view = view;
            this.connectionId = connectionId;
            this.rootNodeId = rootNodeId;
        }
    }

    private static final Map<Long, Root> sRoots = new ConcurrentHashMap<>();

    @SuppressWarnings("unchecked")
    private static List<ViewRootImpl> viewRoots() {
        try {
            WindowManagerGlobal g = WindowManagerGlobal.getInstance();
            Field roots = WindowManagerGlobal.class.getDeclaredField("mRoots");
            Field lock = WindowManagerGlobal.class.getDeclaredField("mLock");
            roots.setAccessible(true);
            lock.setAccessible(true);
            synchronized (lock.get(g)) {
                return new ArrayList<>((List<ViewRootImpl>) roots.get(g));
            }
        } catch (ReflectiveOperationException | RuntimeException e) {
            return new ArrayList<>();
        }
    }

    static long hwndOf(IBinder windowToken) {
        WindowSession session = WindowSession.get();
        return session != null && windowToken != null ? session.hwndOf(windowToken) : 0;
    }

    /** Runs {@code task} on the view's UI thread and waits (up to 3 s); null on failure. */
    private static <T> T onUiThread(View view, java.util.concurrent.Callable<T> task) {
        Handler h = view.getHandler();
        if (h == null) return null;
        if (h.getLooper().isCurrentThread()) {
            try {
                return task.call();
            } catch (Exception e) {
                return null;
            }
        }
        FutureTask<T> f = new FutureTask<>(task);
        h.post(f);
        try {
            return f.get(3, TimeUnit.SECONDS);
        } catch (Exception e) {
            return null;
        }
    }

    private static Root root(long hwnd) {
        Root r = sRoots.get(hwnd);
        if (r != null && r.view.isAttachedToWindow() && hwndOf(r.view.getWindowToken()) == hwnd) {
            return r;
        }
        sRoots.remove(hwnd);
        for (ViewRootImpl vr : viewRoots()) {
            View v = vr.getView();
            if (v == null || !v.isAttachedToWindow() || hwndOf(v.getWindowToken()) != hwnd) continue;
            Integer id = onUiThread(v, vr::getDirectAccessibilityConnectionId);
            if (id == null) return null;
            AccessibilityNodeInfo info = AccessibilityInteractionClient.getInstance()
                    .findAccessibilityNodeInfoByAccessibilityId(id,
                            AccessibilityWindowInfo.ANY_WINDOW_ID, AccessibilityNodeInfo.ROOT_NODE_ID,
                            true, 0, null);
            if (info == null) return null;
            r = new Root(vr, v, id, info.getSourceNodeId());
            sRoots.put(hwnd, r);
            return r;
        }
        return null;
    }

    private static AccessibilityNodeInfo find(Root r, long id) {
        return AccessibilityInteractionClient.getInstance().findAccessibilityNodeInfoByAccessibilityId(
                r.connectionId, AccessibilityWindowInfo.ANY_WINDOW_ID,
                id == ROOT ? AccessibilityNodeInfo.ROOT_NODE_ID : id, true, 0, null);
    }

    private static long publicId(Root r, long id) {
        return id == r.rootNodeId ? ROOT : id;
    }

    private static final int ENABLED = 1, FOCUSABLE = 2, FOCUSED = 4, CLICKABLE = 8,
            CHECKABLE = 16, CHECKED = 32, EDITABLE = 64, PASSWORD = 128, VISIBLE = 256,
            SCROLLABLE = 512, HEADING = 1024, SELECTED = 2048, LONG_CLICKABLE = 4096;

    private static String clean(CharSequence s) {
        return s == null ? "" : s.toString().replace('\t', ' ').replace('\n', ' ').replace('\r', ' ');
    }

    /** The WebView a node is (its host node), else null. */
    private static WebView webViewOf(Root r, AccessibilityNodeInfo n) {
        if (!"android.webkit.WebView".contentEquals(String.valueOf(n.getClassName()))) return null;
        long id = n.getSourceNodeId();
        // A real View uses HOST_VIEW_ID (-1); UNDEFINED_ITEM_ID is the
        // unrelated invalid view/virtual id (Integer.MAX_VALUE).
        if (AccessibilityNodeInfo.getVirtualDescendantId(id) != AccessibilityNodeProvider.HOST_VIEW_ID) {
            return null;
        }
        int viewId = AccessibilityNodeInfo.getAccessibilityViewId(id);
        View v = onUiThread(r.view, () -> r.view.findViewByAccessibilityIdTraversal(viewId));
        return v instanceof WebView ? (WebView) v : null;
    }

    private static String describe(Root r, AccessibilityNodeInfo n) {
        int[] origin = new int[2];
        r.view.getLocationOnScreen(origin);
        Rect b = new Rect();
        n.getBoundsInScreen(b);
        b.offset(-origin[0], -origin[1]);
        int flags = 0;
        if (n.isEnabled()) flags |= ENABLED;
        if (n.isFocusable()) flags |= FOCUSABLE;
        if (n.isFocused()) flags |= FOCUSED;
        if (n.isClickable()) flags |= CLICKABLE;
        if (n.isLongClickable()) flags |= LONG_CLICKABLE;
        if (n.isCheckable()) flags |= CHECKABLE;
        if (n.isChecked()) flags |= CHECKED;
        if (n.isEditable()) flags |= EDITABLE;
        if (n.isPassword()) flags |= PASSWORD;
        if (n.isVisibleToUser()) flags |= VISIBLE;
        if (n.isScrollable()) flags |= SCROLLABLE;
        if (n.isHeading()) flags |= HEADING;
        if (n.isSelected()) flags |= SELECTED;
        String name;
        String value = "";
        String help = clean(n.getHintText());
        if (n.isEditable()) {
            name = clean(n.getContentDescription());
            if (name.isEmpty()) name = help;
            value = n.isShowingHintText() ? "" : clean(n.getText());
        } else {
            name = clean(n.getContentDescription());
            if (name.isEmpty()) name = clean(n.getText());
            CharSequence state = n.getStateDescription();
            if (state != null && state.length() > 0) help = help.isEmpty() ? clean(state) : help;
        }
        StringBuilder children = new StringBuilder();
        long hosted = 0;
        WebView web = webViewOf(r, n);
        if (web != null) {
            hosted = com.gesher.app.webview.GesherWebView.containerHwnd(web);
        }
        if (hosted == 0) {
            for (int i = 0; i < n.getChildCount(); i++) {
                if (children.length() > 0) children.append(',');
                children.append(publicId(r, n.getChildId(i)));
            }
        }
        long parent = n.getParentNodeId();
        parent = parent == AccessibilityNodeInfo.UNDEFINED_NODE_ID ? 0 : publicId(r, parent);
        long id = publicId(r, n.getSourceNodeId());
        if (id == ROOT) parent = 0;
        return id + "\t" + parent + "\t" + clean(n.getClassName()) + "\t" + name + "\t" + value
                + "\t" + help + "\t" + flags + "\t" + b.left + " " + b.top + " " + b.right + " "
                + b.bottom + "\t" + children + "\t" + hosted + "\t"
                + clean(n.getViewIdResourceName());
    }

    // ---- Called by the natives (UI Automation's threads) ----

    static String node(long hwnd, long id) {
        try {
            Root r = root(hwnd);
            if (r == null) return null;
            AccessibilityNodeInfo n = find(r, id);
            return n != null ? describe(r, n) : null;
        } catch (RuntimeException e) {
            AccessibilityService.log("node " + id + ": " + e);
            return null;
        }
    }

    /** The deepest visible node at a point of the window's client area, or 0. */
    static long nodeAt(long hwnd, int x, int y) {
        try {
            Root r = root(hwnd);
            if (r == null) return 0;
            int[] origin = new int[2];
            r.view.getLocationOnScreen(origin);
            int sx = x + origin[0], sy = y + origin[1];
            long best = 0;
            ArrayDeque<Long> queue = new ArrayDeque<>();
            queue.add(ROOT);
            Rect b = new Rect();
            for (int seen = 0; !queue.isEmpty() && seen < 2000; seen++) {
                long id = queue.poll();
                AccessibilityNodeInfo n = find(r, id);
                if (n == null || !n.isVisibleToUser()) continue;
                n.getBoundsInScreen(b);
                if (!b.contains(sx, sy)) continue;
                best = publicId(r, n.getSourceNodeId());
                if (webViewOf(r, n) != null) continue;
                for (int i = 0; i < n.getChildCount(); i++) queue.add(n.getChildId(i));
            }
            return best;
        } catch (RuntimeException e) {
            return 0;
        }
    }

    static long focused(long hwnd) {
        try {
            Root r = root(hwnd);
            if (r == null) return 0;
            AccessibilityNodeInfo n = AccessibilityInteractionClient.getInstance().findFocus(
                    r.connectionId, AccessibilityWindowInfo.ANY_WINDOW_ID,
                    AccessibilityNodeInfo.ROOT_NODE_ID, AccessibilityNodeInfo.FOCUS_INPUT);
            return n != null ? publicId(r, n.getSourceNodeId()) : 0;
        } catch (RuntimeException e) {
            return 0;
        }
    }

    /** action: 1 click, 2 focus, 3 set the text, 5 show on screen. */
    static boolean perform(long hwnd, long id, int action, String text) {
        try {
            Root r = root(hwnd);
            if (r == null) return false;
            int a;
            Bundle args = null;
            switch (action) {
                case 1: a = AccessibilityNodeInfo.ACTION_CLICK; break;
                case 2: a = AccessibilityNodeInfo.ACTION_FOCUS; break;
                case 3:
                    a = AccessibilityNodeInfo.ACTION_SET_TEXT;
                    args = new Bundle();
                    args.putCharSequence(
                            AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, text);
                    break;
                case 5:
                    a = AccessibilityNodeInfo.AccessibilityAction.ACTION_SHOW_ON_SCREEN.getId();
                    break;
                default: return false;
            }
            boolean ok = AccessibilityInteractionClient.getInstance().performAccessibilityAction(
                    r.connectionId, AccessibilityWindowInfo.ANY_WINDOW_ID,
                    id == ROOT ? AccessibilityNodeInfo.ROOT_NODE_ID : id, a, args);
            AccessibilityService.log("UI Automation: action " + action + " on node " + id
                    + (ok ? "" : " (not done)"));
            return ok;
        } catch (RuntimeException e) {
            AccessibilityService.log("perform " + action + " on " + id + ": " + e);
            return false;
        }
    }


    // ---- Events ----

    private static final Map<Long, Long> sLastStructure = new ConcurrentHashMap<>();

    /** An AccessibilityEvent of a registered window, as a UIA event. */
    static void onEvent(AccessibilityEvent e, IBinder windowToken) {
        if (sLoaded == null || !sLoaded) return;
        long hwnd = hwndOf(windowToken);
        if (hwnd == 0) return;
        Root r = sRoots.get(hwnd);
        long id = e.getSourceNodeId();
        if (r != null) id = publicId(r, id);
        StringBuilder text = new StringBuilder();
        for (CharSequence t : e.getText()) {
            if (text.length() > 0) text.append(' ');
            text.append(t);
        }
        switch (e.getEventType()) {
            case AccessibilityEvent.TYPE_VIEW_FOCUSED:
            case AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED:
                raise(hwnd, id, 1, null);
                break;
            case AccessibilityEvent.TYPE_ANNOUNCEMENT:
                AccessibilityService.log("announcement for UI Automation: " + text);
                raise(hwnd, id, 3, text.toString());
                break;
            case AccessibilityEvent.TYPE_VIEW_CLICKED:
                raise(hwnd, id, 4, null);
                break;
            case AccessibilityEvent.TYPE_VIEW_TEXT_CHANGED:
                raise(hwnd, id, 5, text.toString());
                break;
            case AccessibilityEvent.TYPE_WINDOW_STATE_CHANGED:
            case AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED: {
                long now = android.os.SystemClock.uptimeMillis();
                Long last = sLastStructure.get(hwnd);
                if (last != null && now - last < 250) break;
                sLastStructure.put(hwnd, now);
                raise(hwnd, 0, 2, null);
                break;
            }
            default:
                break;
        }
    }
}
