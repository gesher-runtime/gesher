package com.gesher.app.webview;

import java.util.ArrayDeque;
import java.util.Iterator;

/**
 * The WebView's own navigations (loadUrl, loadData, loadDataWithBaseURL, postUrl) that
 * WebView2 has not started yet, in the order they were issued, each with its WebView2 URL
 * and its number (WebView2Host hands it to the native host with the Navigate).
 *
 * <p>Android does not ask WebViewClient.shouldOverrideUrlLoading about these loads. Two of
 * them can be issued before WebView2 raises the first one's NavigationStarting (an app
 * that calls loadDataWithBaseURL twice in a row): WebView2 then starts both, the first
 * superseded by the second, or only the second. Each NavigationStarting that is not a
 * redirect takes the oldest issued entry with its URL; the entries before it were
 * superseded before they started. A main-frame navigation that ends without being
 * superseded ends every entry that was handed to WebView2 before it started: those
 * started (or were superseded) already, so a later page navigation to the same URL is
 * the page's. Thread-safe: issued on the WebView's Looper, the rest on the WebView thread.
 */
final class ApiNavigations {
    /** More outstanding loads than this were superseded long ago. */
    static final int LIMIT = 32;

    private static final class Entry {
        final long number;
        final String url;

        Entry(long number, String url) {
            this.number = number;
            this.url = url;
        }
    }

    private final ArrayDeque<Entry> mIssued = new ArrayDeque<>();
    private long mLast;

    /** The WebView asks WebView2 to load {@code url}: the load's number (0 for none). */
    synchronized long issued(String url) {
        if (url == null) return 0;
        if (mIssued.size() == LIMIT) mIssued.removeFirst();
        Entry entry = new Entry(++mLast, url);
        mIssued.addLast(entry);
        return entry.number;
    }

    /**
     * A main-frame NavigationStarting (not a redirect) for {@code url}: whether it is one
     * of the WebView's own loads. That entry and the older ones are taken.
     */
    synchronized boolean started(String url) {
        if (url == null) return false;
        boolean found = false;
        for (Entry e : mIssued) {
            if (url.equals(e.url)) {
                found = true;
                break;
            }
        }
        if (!found) return false;
        while (!url.equals(mIssued.removeFirst().url)) {
            // Superseded before it started.
        }
        return true;
    }

    /** {@code url}'s load will not start (Navigate failed, or it was a fragment change). */
    synchronized void abandoned(String url) {
        if (url == null) return;
        Iterator<Entry> i = mIssued.iterator();
        while (i.hasNext()) {
            if (url.equals(i.next().url)) {
                i.remove();
                return;
            }
        }
    }

    /**
     * A main-frame navigation ended (it was not superseded); WebView2 had been handed the
     * loads up to {@code handed} when it started. Those have started or will not start.
     */
    synchronized void ended(long handed) {
        while (!mIssued.isEmpty() && mIssued.peekFirst().number <= handed) {
            mIssued.removeFirst();
        }
    }

    synchronized int size() {
        return mIssued.size();
    }
}
