/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.webkit.WebStorage;
import com.gesher.app.webview.WebView2Host;
import java.lang.reflect.Constructor;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONArray;
import org.json.JSONObject;

/** Real public WebStorage calls, JNI and WebView2; every request is answered locally. */
public final class WebStorageOriginContractTest implements WebView2Host.Client {
    private static final String FIRST = "https://first.storage-contract.invalid";
    private static final String SECOND = "https://second.storage-contract.invalid";
    private static final String DATABASE = "gesher-origin-contract";
    private static int checks;
    private final AtomicLong next = new AtomicLong(1);
    private final ConcurrentHashMap<Long, LinkedBlockingQueue<Result>> pending =
            new ConcurrentHashMap<>();
    private final LinkedBlockingQueue<Result> ready = new LinkedBlockingQueue<>();
    private final LinkedBlockingQueue<Result> navigated = new LinkedBlockingQueue<>();
    private final AtomicReference<Throwable> failure = new AtomicReference<>();
    private volatile WebView2Host host;

    private static final class Result {
        final int status;
        final String value;
        Result(int status, String value) { this.status = status; this.value = value; }
    }

    private static synchronized void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }

    @Override
    public void onEvent(int type, long id, int arg, String a, String b, String c, byte[] bytes) {
        Result result = new Result(arg, a);
        if (type == WebView2Host.EVENT_READY) ready.add(result);
        else if (type == WebView2Host.EVENT_PAGE_FINISHED) navigated.add(result);
        else if (type == WebView2Host.EVENT_DEVTOOLS_RESULT) {
            LinkedBlockingQueue<Result> queue = pending.get(id);
            if (queue != null) queue.add(result);
        } else if (type == WebView2Host.EVENT_FAILED || type == WebView2Host.EVENT_PROCESS_FAILED) {
            failure.compareAndSet(null, new AssertionError("WebView2 event " + type + ": " + a));
            ready.add(result);
            navigated.add(result);
        }
    }

    @Override public boolean onNavigationStarting(String url, boolean user, boolean redirect, long handed) {
        return false;
    }

    @Override
    public void onResourceRequest(long id, String url, String method, String headers,
            int context, boolean mainFrame) {
        boolean owned = url.startsWith(FIRST + "/") || url.startsWith(SECOND + "/");
        host.respond(id, owned ? 200 : 403, owned ? "OK" : "Forbidden",
                "Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n",
                "<!doctype html><title>Storage contract</title>".getBytes(StandardCharsets.UTF_8));
    }

    @Override public String onBridgeCall(String object, String method, String args) { return "{}"; }
    @Override public void onBridgeRequest(long id, String body) {
        host.respond(id, 404, "Not Found", "", new byte[0]);
    }

    private Result await(LinkedBlockingQueue<Result> queue, String operation) throws Exception {
        Result result = queue.poll(15, TimeUnit.SECONDS);
        if (failure.get() != null) throw new AssertionError(operation, failure.get());
        check(result != null, operation + " timed out");
        return result;
    }

    private JSONObject devtools(String method, JSONObject parameters) throws Exception {
        long id = next.getAndIncrement();
        LinkedBlockingQueue<Result> queue = new LinkedBlockingQueue<>();
        pending.put(id, queue);
        try {
            host.callDevTools(method, parameters.toString(), id);
            Result result = await(queue, method);
            check(result.status >= 0, method + " failed: " + result.status + " " + result.value);
            JSONObject object = new JSONObject(result.value);
            check(!object.has("error"), method + " returned an error: " + result.value);
            return object;
        } finally { pending.remove(id); }
    }

    private Object evaluate(String expression) throws Exception {
        JSONObject response = devtools("Runtime.evaluate", new JSONObject()
                .put("expression", expression).put("awaitPromise", true).put("returnByValue", true));
        check(!response.has("exceptionDetails"), "JavaScript failed: " + response);
        return response.getJSONObject("result").get("value");
    }

    private void navigate(String origin) throws Exception {
        navigated.clear();
        host.navigate(origin + "/", "");
        Result result = await(navigated, "navigate " + origin);
        check(result.status == -1, "navigation failed: " + result.status);
        check((origin + "/").equals(result.value), "unexpected document: " + result.value);
    }

    private boolean hasDatabase(String origin) throws Exception {
        JSONArray names = devtools("IndexedDB.requestDatabaseNames", new JSONObject()
                .put("securityOrigin", origin)).getJSONArray("databaseNames");
        for (int i = 0; i < names.length(); i++) {
            if (DATABASE.equals(names.getString(i))) return true;
        }
        return false;
    }

    private void seed(String origin, String marker) throws Exception {
        navigate(origin);
        String value = JSONObject.quote(marker);
        check(Boolean.TRUE.equals(evaluate("(async()=>{"
                + "localStorage.setItem('marker'," + value + ");"
                + "document.cookie='marker=" + marker + "; Path=/; SameSite=Lax';"
                + "await (await caches.open('origin-contract')).put('/cached',new Response(" + value + "));"
                + "await new Promise((ok,fail)=>{const r=indexedDB.open('" + DATABASE + "',1);"
                + "r.onupgradeneeded=()=>r.result.createObjectStore('data');r.onerror=()=>fail(r.error);"
                + "r.onsuccess=()=>{const d=r.result,t=d.transaction('data','readwrite');"
                + "t.objectStore('data').put(" + value + ",'marker');"
                + "t.oncomplete=()=>{d.close();ok();};t.onerror=()=>{d.close();fail(t.error);};};});"
                + "return true;})()")), "storage seed failed");
        check(hasDatabase(origin), "seeded database missing at " + origin);
    }

    private void preserved(String origin, String marker, boolean database) throws Exception {
        navigate(origin);
        String value = JSONObject.quote(marker);
        check(Boolean.TRUE.equals(evaluate("(async()=>{const cached=await caches.match('/cached');"
                + "return localStorage.getItem('marker')===" + value
                + "&&document.cookie.split('; ').includes('marker=" + marker + "')"
                + "&&!!cached&&await cached.text()===" + value + ";})()")),
                "local storage, cookies or cache changed at " + origin);
        check(hasDatabase(origin) == database, "wrong database scope at " + origin);
        if (database) {
            check(marker.equals(evaluate("new Promise((ok,fail)=>{"
                    + "const r=indexedDB.open('" + DATABASE + "');r.onerror=()=>fail(r.error);"
                    + "r.onsuccess=()=>{const d=r.result,t=d.transaction('data','readonly'),"
                    + "q=t.objectStore('data').get('marker');q.onsuccess=()=>ok(q.result);"
                    + "q.onerror=()=>fail(q.error);t.oncomplete=()=>d.close();};})")),
                    "other origin's IndexedDB value changed");
        }
    }

    private void run() throws Exception {
        host = WebView2Host.create(this, 1.0, 0xffffffff);
        check(host != null, "host creation rejected");
        await(ready, "WebView2 readiness");
        host.setSettings(true, false, false, false, true, false, "");
        devtools("IndexedDB.enable", new JSONObject());
        seed(FIRST, "one");
        seed(SECOND, "two");
        Constructor<?> ctor = Class.forName("com.gesher.app.webview.GesherWebStorage")
                .getDeclaredConstructor();
        ctor.setAccessible(true);
        WebStorage storage = (WebStorage) ctor.newInstance();
        storage.deleteOrigin(null);
        storage.deleteOrigin("");

        // Native admission rejects these before posting any CDP work. The
        // recording native fixture runs old-code negative controls without a
        // browser; never use this real profile to run that negative control.
        LinkedBlockingQueue<Result> done = new LinkedBlockingQueue<>();
        for (String invalid : new String[] { "not an origin\"\n", "null", "file://",
                "custom://example.invalid", "https://", "https://example.invalid:65536",
                "https://example.invalid/path", "https://example.invalid\u0000ignored" }) {
            check(!WebView2Host.clearOriginData(invalid, (type, status, value) ->
                    done.add(new Result(status, value))), "invalid origin was admitted");
            storage.deleteOrigin(invalid);
        }
        check(done.poll(50, TimeUnit.MILLISECONDS) == null, "rejected request called back");
        preserved(FIRST, "one", true);
        preserved(SECOND, "two", true);

        storage.deleteOrigin(FIRST);
        // IndexedDB's inspector queries require an inspected document at this origin.
        navigate(FIRST);
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(15);
        while (hasDatabase(FIRST) && System.nanoTime() < deadline) Thread.sleep(25);
        check(!hasDatabase(FIRST), "target origin's IndexedDB was not removed");
        preserved(FIRST, "one", false);
        preserved(SECOND, "two", true);

        // The internal completion boundary also handles normalized tuple origins.
        check(WebView2Host.clearOriginData("HTTPS://FIRST.STORAGE-CONTRACT.INVALID:443/",
                (type, status, value) -> done.add(new Result(status, value))),
                "valid canonicalized origin was rejected");
        check(await(done, "origin completion").status >= 0, "origin deletion failed");
        check(done.poll(50, TimeUnit.MILLISECONDS) == null, "completion delivered twice");
        preserved(FIRST, "one", false);
        preserved(SECOND, "two", true);
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 1 || args[0].isEmpty()) {
            throw new IllegalArgumentException("Pass a private disposable WebView2 profile path");
        }
        System.loadLibrary("android_runtime");
        check(WebView2Host.load(), "WebView2 bridge unavailable: " + WebView2Host.loadError());
        WebView2Host.configure(args[0], "");
        WebStorageOriginContractTest fixture = new WebStorageOriginContractTest();
        try { fixture.run(); }
        finally {
            if (fixture.host != null) fixture.host.destroy();
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(15);
            boolean closed;
            do {
                closed = WebView2Host.shutdownIdle(15000);
                if (!closed) Thread.sleep(25);
            } while (!closed && System.nanoTime() < deadline);
            check(closed, "WebView2 did not shut down after host destruction");
        }
        System.out.println("WebStorageOriginContractTest all passed: " + checks
                + " checks; two origins, IndexedDB, localStorage, cookies and cache");
    }
}
