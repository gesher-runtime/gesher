package com.gesher.app.webview;

import android.graphics.Rect;
import android.os.Bundle;
import android.view.View;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;

import com.gesher.app.accessibility.UiaClient;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * A WebView's page in Android's accessibility tree (ADR 0058): its
 * AccessibilityNodeProvider, whose virtual views are the elements of WebView2's UI
 * Automation tree (Chromium's accessibility of the page), read under the WebView2 window
 * ({@link UiaClient}, refreshed asynchronously after 500 ms). Control types map to Android's
 * classes (a button is android.widget.Button, an edit box an EditText...), names to text,
 * UIA states to the node's; clicks, focus, text and scrolling act on the elements
 * (Invoke/Toggle, SetFocus, Value.SetValue, ScrollItem).
 */
final class WebViewAccessibility extends AccessibilityNodeProvider {
    private static final long MAX_AGE_MS = 500;
    private static final ExecutorService sWorker = Executors.newSingleThreadExecutor(r -> {
        Thread t = new Thread(r, "WebViewAccessibility");
        t.setDaemon(true);
        return t;
    });

    private final GesherWebViewProvider mProvider;
    private UiaClient.Snapshot mSnapshot;
    private long mSnapshotAt;
    private boolean mRefreshing;

    WebViewAccessibility(GesherWebViewProvider provider) {
        mProvider = provider;
    }

    private UiaClient.Snapshot snapshot() {
        long now = android.os.SystemClock.uptimeMillis();
        if (!mRefreshing && (mSnapshot == null || now - mSnapshotAt > MAX_AGE_MS)) {
            WebView2Host host = mProvider.host();
            long container = host != null ? host.containerHwnd() : 0;
            if (container == 0) return mSnapshot;
            mRefreshing = true;
            // UIA can query this window's Android provider while locating the
            // embedded HWND. Keep the UI thread free to answer that query.
            sWorker.execute(() -> {
                UiaClient.Snapshot s = UiaClient.snapshot(container, 1500, 3000);
                mProvider.view().post(() -> {
                    mRefreshing = false;
                    mSnapshotAt = android.os.SystemClock.uptimeMillis();
                    if (s != null) {
                        mSnapshot = s;
                        if (GesherWebView.DEBUG) {
                            GesherWebView.log("accessibility: " + s.elements.size()
                                    + " UI Automation element(s) under the WebView2 window");
                        }
                        mProvider.view().sendAccessibilityEvent(
                                AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED);
                    }
                });
            });
        }
        return mSnapshot;
    }

    /** Windows screen coordinates as Android's (the view's window on Android's screen). */
    private void toAndroid(Rect windows, Rect out) {
        View view = mProvider.view();
        long hwnd = GesherWebView.hwndOf(view);
        int[] client = UiaClient.clientOrigin(hwnd);
        int[] root = new int[2];
        view.getRootView().getLocationOnScreen(root);
        out.set(windows);
        out.offset(root[0] - client[0], root[1] - client[1]);
    }

    private static String className(UiaClient.Element e) {
        switch (e.controlType) {
            case UiaClient.BUTTON: return "android.widget.Button";
            case UiaClient.CHECK_BOX: return "android.widget.CheckBox";
            case UiaClient.RADIO_BUTTON: return "android.widget.RadioButton";
            case UiaClient.EDIT: return "android.widget.EditText";
            case UiaClient.COMBO_BOX: return "android.widget.Spinner";
            case UiaClient.IMAGE: return "android.widget.Image";
            case UiaClient.LIST: return "android.widget.ListView";
            case UiaClient.PROGRESS_BAR: return "android.widget.ProgressBar";
            case UiaClient.SLIDER: return "android.widget.SeekBar";
            case UiaClient.TEXT: return "android.widget.TextView";
            case UiaClient.TABLE: return "android.widget.GridView";
            case UiaClient.DOCUMENT: return "android.webkit.WebView";
            default: return "android.view.View";
        }
    }

    @Override
    public AccessibilityNodeInfo createAccessibilityNodeInfo(int virtualViewId) {
        View view = mProvider.view();
        UiaClient.Snapshot s = snapshot();
        if (virtualViewId == HOST_VIEW_ID) {
            AccessibilityNodeInfo info = AccessibilityNodeInfo.obtain(view);
            view.onInitializeAccessibilityNodeInfo(info);
            if (s != null) {
                for (int id : s.top) info.addChild(view, id);
            }
            return info;
        }
        UiaClient.Element e = s != null ? s.get(virtualViewId) : null;
        if (e == null) return null;
        AccessibilityNodeInfo info = AccessibilityNodeInfo.obtain(view, virtualViewId);
        info.setPackageName(view.getContext().getPackageName());
        if (e.parent < 0) info.setParent(view); else info.setParent(view, e.parent);
        info.setClassName(className(e));
        if (e.has(UiaClient.EDITABLE)) {
            info.setEditable(true);
            info.setText(e.value);
            if (!e.name.isEmpty()) info.setHintText(e.name);
            info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SET_TEXT);
        } else if (!e.name.isEmpty()) {
            info.setText(e.name);
        } else if (!e.value.isEmpty()) {
            info.setText(e.value);
        }
        if (!e.help.isEmpty() && !e.has(UiaClient.EDITABLE)) info.setTooltipText(e.help);
        if (!e.automationId.isEmpty()) info.setViewIdResourceName(e.automationId);
        Rect r = new Rect();
        toAndroid(e.bounds, r);
        info.setBoundsInScreen(r);
        info.setEnabled(e.has(UiaClient.ENABLED));
        info.setFocusable(e.has(UiaClient.FOCUSABLE));
        info.setFocused(e.has(UiaClient.FOCUSED));
        info.setVisibleToUser(e.has(UiaClient.VISIBLE));
        info.setPassword(e.has(UiaClient.PASSWORD));
        info.setScrollable(e.has(UiaClient.SCROLLABLE));
        info.setHeading(e.has(UiaClient.HEADING));
        info.setSelected(e.has(UiaClient.SELECTED));
        boolean clickable = e.has(UiaClient.CLICKABLE) || e.has(UiaClient.CHECKABLE)
                || e.controlType == UiaClient.HYPERLINK;
        info.setClickable(clickable);
        if (clickable) info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_CLICK);
        if (e.has(UiaClient.CHECKABLE)) {
            info.setCheckable(true);
            info.setChecked(e.has(UiaClient.CHECKED));
        }
        if (e.has(UiaClient.FOCUSABLE)) {
            info.addAction(e.has(UiaClient.FOCUSED)
                    ? AccessibilityNodeInfo.AccessibilityAction.ACTION_CLEAR_FOCUS
                    : AccessibilityNodeInfo.AccessibilityAction.ACTION_FOCUS);
        }
        info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SHOW_ON_SCREEN);
        for (int child : e.children) info.addChild(view, child);
        return info;
    }

    @Override
    public boolean performAction(int virtualViewId, int action, Bundle arguments) {
        View view = mProvider.view();
        if (virtualViewId == HOST_VIEW_ID) return view.performAccessibilityAction(action, arguments);
        UiaClient.Snapshot s = snapshot();
        if (s == null || s.get(virtualViewId) == null) return false;
        int command;
        String text = null;
        switch (action) {
            case AccessibilityNodeInfo.ACTION_CLICK:
                command = UiaClient.CLICK;
                break;
            case AccessibilityNodeInfo.ACTION_FOCUS:
                command = UiaClient.FOCUS;
                break;
            case AccessibilityNodeInfo.ACTION_SET_TEXT: {
                CharSequence t = arguments != null ? arguments.getCharSequence(
                        AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE) : null;
                command = UiaClient.SET_VALUE;
                text = t != null ? t.toString() : "";
                break;
            }
            default:
                if (action == AccessibilityNodeInfo.AccessibilityAction.ACTION_SHOW_ON_SCREEN.getId()) {
                    command = UiaClient.SCROLL_INTO_VIEW;
                } else {
                    return false;
                }
                break;
        }
        int nativeAction = command;
        String value = text;
        sWorker.execute(() -> {
            boolean ok = UiaClient.act(s, virtualViewId, nativeAction, value);
            view.post(() -> {
                GesherWebView.log("accessibility: action " + action + " on the page's element "
                        + virtualViewId + " ('" + s.get(virtualViewId).name + "')"
                        + (ok ? "" : ": not done"));
                mSnapshotAt = 0;
                snapshot();
            });
        });
        return true;  // Accepted; the page's resulting state arrives asynchronously.
    }

    @Override
    public List<AccessibilityNodeInfo> findAccessibilityNodeInfosByText(String text,
            int virtualViewId) {
        List<AccessibilityNodeInfo> out = new ArrayList<>();
        UiaClient.Snapshot s = snapshot();
        if (s == null || text == null) return out;
        String needle = text.toLowerCase(java.util.Locale.ROOT);
        for (UiaClient.Element e : s.elements) {
            if (e.name.toLowerCase(java.util.Locale.ROOT).contains(needle)
                    || e.value.toLowerCase(java.util.Locale.ROOT).contains(needle)) {
                AccessibilityNodeInfo info = createAccessibilityNodeInfo(e.id);
                if (info != null) out.add(info);
            }
        }
        return out;
    }
}
