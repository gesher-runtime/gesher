package com.gesher.app.webview;

import android.os.Handler;
import android.os.Looper;
import android.webkit.WebMessage;
import android.webkit.WebMessagePort;

import java.security.SecureRandom;
import java.util.ArrayDeque;
import java.util.IdentityHashMap;

/** One endpoint of a local or WebView2-backed message channel. */
final class GesherWebMessagePort extends WebMessagePort {
    private static final SecureRandom sRandom = new SecureRandom();
    // A transfer can rewire both ends of the same channel. Validate and move all
    // endpoints atomically, without locking application port arrays in any order.
    private static final Object sLock = new Object();

    final String mId;
    private GesherWebMessagePort mPeer;
    private GesherWebViewProvider mPage;
    private boolean mClosed;
    private boolean mTransferred;
    private boolean mStarted;
    private WebMessageCallback mCallback;
    private Handler mHandler;
    private boolean mDeliveryScheduled;
    private final ArrayDeque<WebMessage> mQueue = new ArrayDeque<>();

    private GesherWebMessagePort(String id) {
        mId = id;
    }

    private GesherWebMessagePort() {
        this(Long.toHexString(sRandom.nextLong()) + Long.toHexString(sRandom.nextLong()));
    }

    static WebMessagePort[] pair() {
        GesherWebMessagePort a = new GesherWebMessagePort();
        GesherWebMessagePort b = new GesherWebMessagePort();
        a.mPeer = b;
        b.mPeer = a;
        return new WebMessagePort[] {a, b};
    }

    private void requireUsable() {
        if (mClosed || mTransferred) {
            throw new IllegalStateException("Port is already closed or transferred");
        }
    }

    /** Validate the complete list before transferring any endpoint. sLock is held. */
    private static GesherWebMessagePort[] transferable(WebMessagePort[] ports,
                                                       GesherWebMessagePort source) {
        if (ports == null) return null;
        GesherWebMessagePort[] result = new GesherWebMessagePort[ports.length];
        IdentityHashMap<GesherWebMessagePort, Boolean> seen = new IdentityHashMap<>();
        for (int i = 0; i < ports.length; i++) {
            if (!(ports[i] instanceof GesherWebMessagePort)) {
                throw new IllegalArgumentException("not a port of this WebView");
            }
            GesherWebMessagePort port = (GesherWebMessagePort) ports[i];
            port.requireUsable();
            if (port == source) throw new IllegalStateException("Source port cannot be transferred");
            if (port.mStarted) throw new IllegalStateException("Port is already started");
            if (seen.put(port, Boolean.TRUE) != null) {
                throw new IllegalStateException("Port is listed more than once");
            }
            result[i] = port;
        }
        return result;
    }

    /** Transfer a complete list to a page; the caller registers it before flushing. */
    static GesherWebMessagePort[] transferToPage(WebMessagePort[] ports,
                                                GesherWebViewProvider page) {
        synchronized (sLock) {
            GesherWebMessagePort[] result = transferable(ports, null);
            if (result == null) return new GesherWebMessagePort[0];
            for (GesherWebMessagePort port : result) {
                port.mTransferred = true;
                port.mPage = page;
            }
            return result;
        }
    }

    /** Transfer the endpoint, not the Java wrapper that has just been detached. */
    private static WebMessage localMessage(WebMessage message, GesherWebMessagePort[] ports) {
        if (ports == null) return message;
        WebMessagePort[] received = new WebMessagePort[ports.length];
        for (int i = 0; i < ports.length; i++) {
            GesherWebMessagePort old = ports[i];
            GesherWebMessagePort next = new GesherWebMessagePort(old.mId);
            next.mPeer = old.mPeer;
            if (next.mPeer != null) next.mPeer.mPeer = next;
            next.mQueue.addAll(old.mQueue);
            old.mQueue.clear();
            old.mPeer = null;
            old.mTransferred = true;
            received[i] = next;
        }
        return WebMessagePayload.binary(message)
                ? new WebMessagePayload(WebMessagePayload.bytes(message), received)
                : new WebMessage(message.getData(), received);
    }

    /** Called after the page receives the message that creates its new ports. */
    void flushToPage() {
        ArrayDeque<WebMessage> queued;
        GesherWebViewProvider page;
        synchronized (sLock) {
            if (mClosed || mPage == null || mQueue.isEmpty()) return;
            page = mPage;
            queued = new ArrayDeque<>(mQueue);
            mQueue.clear();
        }
        for (WebMessage message : queued) page.postToPagePort(this, message);
    }

    void pageGone() {
        synchronized (sLock) {
            mClosed = true;
            mPage = null;
            mCallback = null;
            mHandler = null;
            mQueue.clear();
        }
    }

    void fromPage(WebMessage message) {
        GesherWebMessagePort peer;
        synchronized (sLock) {
            if (mClosed) return;
            peer = mPeer;
        }
        // Android permits transferring both endpoints. receive() can route the
        // peer through a different page instead of waiting for a Java callback.
        if (peer != null) peer.receive(message);
    }

    @Override
    public void postMessage(WebMessage message) {
        GesherWebMessagePort peer;
        GesherWebViewProvider page;
        synchronized (sLock) {
            requireUsable();
            if (message == null) throw new NullPointerException("message");
            GesherWebMessagePort[] ports = transferable(message.getPorts(), this);
            mStarted = true;
            peer = mPeer;
            page = peer == null || peer.mClosed ? null : peer.mPage;
            if (page == null) message = localMessage(message, ports);
        }
        if (peer == null) return;
        if (page != null) page.postToPagePort(peer, message);
        else peer.receive(message);
    }

    private void receive(WebMessage message) {
        GesherWebViewProvider page;
        synchronized (sLock) {
            if (mClosed) return;
            page = mPage;
            if (page == null) {
                mQueue.add(message);
                scheduleDelivery();
                return;
            }
        }
        page.postToPagePort(this, message);
    }

    /** sLock is held. Handler queues the task; application callbacks run outside it. */
    private void scheduleDelivery() {
        if (mDeliveryScheduled || mClosed || mCallback == null || mQueue.isEmpty()) return;
        Handler handler = mHandler != null ? mHandler : new Handler(Looper.getMainLooper());
        mDeliveryScheduled = true;
        if (!handler.post(this::deliverNext)) mDeliveryScheduled = false;
    }

    private void deliverNext() {
        WebMessageCallback callback;
        WebMessage message;
        synchronized (sLock) {
            mDeliveryScheduled = false;
            if (mClosed || mTransferred || mCallback == null || mQueue.isEmpty()) return;
            callback = mCallback;
            message = mQueue.remove();
            scheduleDelivery();
        }
        callback.onMessage(this, message);
    }

    @Override
    public void close() {
        GesherWebViewProvider page;
        GesherWebMessagePort peer;
        synchronized (sLock) {
            if (mTransferred) throw new IllegalStateException("Port is already transferred");
            if (mClosed) return;
            mClosed = true;
            mQueue.clear();
            mCallback = null;
            mHandler = null;
            peer = mPeer;
            page = peer == null || peer.mClosed ? null : peer.mPage;
        }
        if (page != null) page.closePagePort(peer);
    }

    @Override
    public void setWebMessageCallback(WebMessageCallback callback) {
        setWebMessageCallback(callback, null);
    }

    @Override
    public void setWebMessageCallback(WebMessageCallback callback, Handler handler) {
        synchronized (sLock) {
            requireUsable();
            mStarted = true;
            mCallback = callback;
            mHandler = handler;
            scheduleDelivery();
        }
    }
}
