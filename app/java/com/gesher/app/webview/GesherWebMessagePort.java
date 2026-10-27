package com.gesher.app.webview;

import android.os.Handler;
import android.os.Looper;
import android.webkit.WebMessage;
import android.webkit.WebMessagePort;

import java.security.SecureRandom;
import java.util.ArrayDeque;

/**
 * One end of a WebView.createWebMessageChannel() pair (docs/webview.md, "Messages"). While
 * both ends are the app's, a message posted to one is delivered to the other's callback.
 * An end sent to a page (WebView.postWebMessage) lives on there as a MessageChannel's
 * port1 (PageScripts.PORTS): what the app posts to the end it kept reaches that port in the
 * page, and what the page posts to it reaches the kept end's callback.
 */
final class GesherWebMessagePort extends WebMessagePort {
    private static final SecureRandom sRandom = new SecureRandom();

    /** The id of this end in the page (when it is sent to one). */
    final String mId;
    private GesherWebMessagePort mPeer;
    /** The WebView whose page holds this end, or null. */
    private GesherWebViewProvider mPage;
    private boolean mClosed;
    private boolean mTransferred;
    private WebMessageCallback mCallback;
    private Handler mHandler;
    private final ArrayDeque<WebMessage> mQueue = new ArrayDeque<>();

    private GesherWebMessagePort() {
        mId = Long.toHexString(sRandom.nextLong()) + Long.toHexString(sRandom.nextLong());
    }

    static WebMessagePort[] pair() {
        GesherWebMessagePort a = new GesherWebMessagePort();
        GesherWebMessagePort b = new GesherWebMessagePort();
        a.mPeer = b;
        b.mPeer = a;
        return new WebMessagePort[] {a, b};
    }

    /** This end goes to the page of {@code page} (postWebMessage's ports). */
    synchronized void transferTo(GesherWebViewProvider page) {
        if (mClosed || mTransferred) {
            throw new IllegalStateException("Port is already closed or transferred");
        }
        mTransferred = true;
        mPage = page;
    }

    /** The page this end was sent to navigated away: it is gone. */
    synchronized void pageGone() {
        mClosed = true;
        mPage = null;
    }

    /** The end that was sent to the page receives a message from the page. */
    void fromPage(WebMessage message) {
        GesherWebMessagePort peer;
        synchronized (this) {
            if (mClosed) return;
            peer = mPeer;
        }
        if (peer != null) peer.receive(message);
    }

    @Override
    public void postMessage(WebMessage message) {
        GesherWebMessagePort peer;
        synchronized (this) {
            if (mClosed || mTransferred) {
                throw new IllegalStateException("Port is already closed or transferred");
            }
            peer = mPeer;
        }
        if (peer == null || message == null) return;
        GesherWebViewProvider page = peer.page();
        if (page != null) {
            page.postToPagePort(peer, message);
        } else {
            WebMessagePort[] ports = message.getPorts();
            if (ports != null) {
                for (WebMessagePort p : ports) {
                    if (p instanceof GesherWebMessagePort) {
                        synchronized (p) {
                            ((GesherWebMessagePort) p).mTransferred = true;
                        }
                    }
                }
            }
            peer.receive(message);
        }
    }

    private synchronized GesherWebViewProvider page() {
        return mClosed ? null : mPage;
    }

    private void receive(WebMessage message) {
        WebMessageCallback callback;
        Handler handler;
        synchronized (this) {
            if (mClosed) return;
            if (mCallback == null) {
                mQueue.add(message);
                return;
            }
            callback = mCallback;
            handler = mHandler;
        }
        deliver(callback, handler, message);
    }

    private void deliver(WebMessageCallback callback, Handler handler, WebMessage message) {
        Handler h = handler != null ? handler : new Handler(Looper.getMainLooper());
        h.post(() -> callback.onMessage(this, message));
    }

    @Override
    public void close() {
        GesherWebViewProvider page;
        GesherWebMessagePort peer;
        synchronized (this) {
            if (mTransferred) throw new IllegalStateException("Port is already transferred");
            if (mClosed) return;
            mClosed = true;
            peer = mPeer;
        }
        page = peer != null ? peer.page() : null;
        if (page != null) page.closePagePort(peer);
    }

    @Override
    public void setWebMessageCallback(WebMessageCallback callback) {
        setWebMessageCallback(callback, null);
    }

    @Override
    public void setWebMessageCallback(WebMessageCallback callback, Handler handler) {
        ArrayDeque<WebMessage> queued;
        synchronized (this) {
            mCallback = callback;
            mHandler = handler;
            if (callback == null || mQueue.isEmpty()) return;
            queued = new ArrayDeque<>(mQueue);
            mQueue.clear();
        }
        for (WebMessage m : queued) deliver(callback, handler, m);
    }
}
