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

    /**
     * Undelivered messages own their freshly transferred endpoints. Retire that
     * ownership graph iteratively: a queued transfer can itself own queued
     * transfers, including cycles. Never follow an ordinary entangled peer or a
     * wrapper whose ownership has already moved to another recipient.
     */
    private static final class Retirement {
        private final ArrayDeque<WebMessage> messages = new ArrayDeque<>();
        private final IdentityHashMap<GesherWebMessagePort, Boolean> seen = new IdentityHashMap<>();
        private final IdentityHashMap<GesherWebMessagePort, GesherWebViewProvider> pages =
                new IdentityHashMap<>();

        private boolean closeOne(GesherWebMessagePort port) {
            if (seen.put(port, Boolean.TRUE) != null || port.mClosed) return false;
            port.mClosed = true;
            port.mPage = null;
            port.mCallback = null;
            port.mHandler = null;
            port.mDeliveryScheduled = false;
            messages.addAll(port.mQueue);
            port.mQueue.clear();
            return true;
        }

        void close(GesherWebMessagePort port, boolean closePagePeer) {
            if (!closeOne(port) || !closePagePeer) return;
            GesherWebMessagePort peer = port.mPeer;
            if (peer != null && !peer.mClosed && peer.mPage != null) {
                pages.put(peer, peer.mPage);
                closeOne(peer);
            }
        }

        void discard(WebMessage message) {
            messages.add(message);
        }

        void drain() {
            while (!messages.isEmpty()) {
                WebMessagePort[] ports = messages.remove().getPorts();
                if (ports == null) continue;
                for (WebMessagePort value : ports) {
                    if (!(value instanceof GesherWebMessagePort)) continue;
                    GesherWebMessagePort port = (GesherWebMessagePort) value;
                    if (!port.mTransferred) close(port, true);
                }
            }
        }

        /** Provider/backend calls must never run under the endpoint graph lock. */
        void finish() {
            for (java.util.Map.Entry<GesherWebMessagePort, GesherWebViewProvider> entry
                    : pages.entrySet()) {
                entry.getValue().closePagePort(entry.getKey());
            }
        }
    }

    /** The message has already consumed its sender's transfer list. */
    static void discardMessage(WebMessage message) {
        Retirement retirement = new Retirement();
        synchronized (sLock) {
            retirement.discard(message);
            retirement.drain();
        }
        retirement.finish();
    }

    boolean isOwnedByPage(GesherWebViewProvider page) {
        synchronized (sLock) {
            return !mClosed && mPage == page;
        }
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
        Retirement retirement = new Retirement();
        synchronized (sLock) {
            retirement.close(this, false);
            retirement.drain();
        }
        retirement.finish();
    }

    void fromPage(WebMessage message) {
        GesherWebMessagePort peer;
        GesherWebViewProvider page = null;
        Retirement retirement = new Retirement();
        synchronized (sLock) {
            peer = mPeer;
            if (mClosed || mPage == null || peer == null) retirement.discard(message);
            else page = peer.receiveLocked(message, retirement);
            retirement.drain();
        }
        retirement.finish();
        // Android permits transferring both endpoints. The peer can route the
        // peer through a different page instead of waiting for a Java callback.
        if (page != null) page.postToPagePort(peer, message);
    }

    @Override
    public void postMessage(WebMessage message) {
        GesherWebMessagePort peer;
        GesherWebViewProvider page = null;
        Retirement retirement = new Retirement();
        synchronized (sLock) {
            requireUsable();
            if (message == null) throw new NullPointerException("message");
            GesherWebMessagePort[] ports = transferable(message.getPorts(), this);
            mStarted = true;
            peer = mPeer;
            boolean doomed = false;
            if (ports != null) {
                for (GesherWebMessagePort port : ports) if (port == peer) doomed = true;
            }
            // Serialization consumes transfers even when there is no recipient,
            // or when the recipient is itself in the transfer list (HTML's
            // message-port-post-message steps 4-6).
            message = localMessage(message, ports);
            if (peer == null || doomed) retirement.discard(message);
            else page = peer.receiveLocked(message, retirement);
            retirement.drain();
        }
        retirement.finish();
        if (page != null) page.postToPagePort(peer, message);
    }

    /** sLock keeps recipient selection, ownership moves and queue insertion atomic. */
    private GesherWebViewProvider receiveLocked(WebMessage message, Retirement retirement) {
        if (mClosed || (mTransferred && mPage == null)) {
            retirement.discard(message);
            return null;
        }
        if (mPage != null) return mPage;
        mQueue.add(message);
        scheduleDelivery();
        return null;
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
        Retirement retirement = new Retirement();
        synchronized (sLock) {
            if (mTransferred) throw new IllegalStateException("Port is already transferred");
            if (mClosed) return;
            retirement.close(this, true);
            retirement.drain();
        }
        retirement.finish();
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
