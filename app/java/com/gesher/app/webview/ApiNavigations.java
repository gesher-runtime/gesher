package com.gesher.app.webview;

import java.util.ArrayDeque;
import java.util.Iterator;

/**
 * The WebView's own navigations (loadUrl, loadData, loadDataWithBaseURL, postUrl) that
 * WebView2 has not started yet, by their WebView2 URL, in the order they were issued.
 *
 * <p>Android does not ask WebViewClient.shouldOverrideUrlLoading about these loads. Two of
 * them can be issued before WebView2 raises the first one's NavigationStarting (an app
 * that calls loadDataWithBaseURL twice in a row): WebView2 then starts both, the first
 * superseded by the second. Each NavigationStarting that is not a redirect takes the
 * oldest issued entry with its URL; the entries before it were superseded before they
 * started. Thread-safe: issued on the WebView's Looper, started on the WebView thread.
 */
final class ApiNavigations {
    /** More outstanding loads than this were superseded long ago. */
    static final int LIMIT = 32;

    private final ArrayDeque<String> mIssued = new ArrayDeque<>();

    /** The WebView asked WebView2 to load {@code url}. */
    synchronized void issued(String url) {
        if (url == null) return;
        if (mIssued.size() == LIMIT) mIssued.removeFirst();
        mIssued.addLast(url);
    }

    /**
     * A main-frame NavigationStarting (not a redirect) for {@code url}: whether it is one
     * of the WebView's own loads. That entry and the older ones are taken.
     */
    synchronized boolean started(String url) {
        if (url == null || !mIssued.contains(url)) return false;
        while (!url.equals(mIssued.removeFirst())) {
            // Superseded before it started.
        }
        return true;
    }

    /** {@code url}'s load will not start (Navigate failed, or it was a fragment change). */
    synchronized void abandoned(String url) {
        if (url == null) return;
        Iterator<String> i = mIssued.iterator();
        while (i.hasNext()) {
            if (url.equals(i.next())) {
                i.remove();
                return;
            }
        }
    }

    synchronized int size() {
        return mIssued.size();
    }
}
