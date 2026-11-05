package com.gesher.app.webview;

import org.json.JSONArray;
import org.json.JSONObject;

import java.nio.charset.StandardCharsets;
import java.util.Map;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;

/** Synchronous renderer calls without blocking WebView2's STA or the app's UI. */
final class JsBridgeDispatcher {
    private static final int MAX_BYTES = 1024 * 1024;
    // Native deferrals expire after 30 seconds. Do not start an already expired call.
    private static final long QUEUE_DEADLINE_NS = TimeUnit.SECONDS.toNanos(25);
    private static final String HEADERS = "Content-Type: application/json; charset=UTF-8\r\n"
            + "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n";
    private static final byte[] FAILED = "{\"e\":\"Java bridge request rejected\"}"
            .getBytes(StandardCharsets.UTF_8);

    private final Map<String, Object> mInterfaces;
    private final boolean mRequireAnnotation;
    private final AtomicLong mGeneration = new AtomicLong();
    private final ThreadPoolExecutor mWorker = new ThreadPoolExecutor(1, 1, 0,
            TimeUnit.MILLISECONDS, new ArrayBlockingQueue<>(16), task -> {
                Thread thread = new Thread(task, "WebView JavaBridge");
                thread.setDaemon(true);
                return thread;
            });
    private volatile boolean mClosed;

    JsBridgeDispatcher(Map<String, Object> interfaces, boolean requireAnnotation) {
        mInterfaces = interfaces;
        mRequireAnnotation = requireAnnotation;
    }

    /** Called on the native event thread, before posting navigation events to the UI. */
    void navigationStarted() {
        mGeneration.incrementAndGet();
    }

    void close() {
        mClosed = true;
        mGeneration.incrementAndGet();
        mWorker.shutdownNow();
    }

    void submit(WebView2Host host, long requestId, String body) {
        if (host == null) return;
        final long generation = mGeneration.get();
        final long deadline = System.nanoTime() + QUEUE_DEADLINE_NS;
        if (mClosed || body == null || body.length() > MAX_BYTES) {
            reject(host, requestId);
            return;
        }
        try {
            mWorker.execute(() -> answer(host, requestId, body, generation, deadline));
        } catch (RejectedExecutionException e) {
            reject(host, requestId);
        }
    }

    private boolean current(WebView2Host host, long generation, long deadline) {
        return !mClosed && !host.isDestroyed() && generation == mGeneration.get()
                && System.nanoTime() - deadline < 0;
    }

    private void answer(WebView2Host host, long id, String body, long generation, long deadline) {
        if (!current(host, generation, deadline)) {
            reject(host, id);
            return;
        }
        byte[] response;
        try {
            JSONObject request = new JSONObject(body);
            Object object = request.opt("object"), method = request.opt("method");
            Object args = request.opt("args");
            if (!Integer.valueOf(1).equals(request.opt("version"))
                    || !(object instanceof String) || !(method instanceof String)
                    || !(args instanceof JSONArray)) {
                reject(host, id);
                return;
            }
            if (!current(host, generation, deadline)) {
                reject(host, id);
                return;
            }
            response = JsBridge.call(mInterfaces, mRequireAnnotation, (String) object,
                    (String) method, args.toString()).getBytes(StandardCharsets.UTF_8);
            if (response.length > MAX_BYTES || !current(host, generation, deadline)) {
                reject(host, id);
                return;
            }
        } catch (Exception e) {
            // No body, argument values or reflected exception messages in diagnostics.
            reject(host, id);
            return;
        }
        host.respond(id, 200, "OK", HEADERS, response);
    }

    private static void reject(WebView2Host host, long id) {
        host.respond(id, 403, "Forbidden", HEADERS, FAILED);
    }
}
