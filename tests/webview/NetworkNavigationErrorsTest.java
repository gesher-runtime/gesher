// Copyright (C) 2026 The Gesher Authors; All rights reserved.
package com.gesher.app.webview;
import org.json.JSONObject;

/** Real production CDP parser/state; JSON is the native browser boundary. */
public final class NetworkNavigationErrorsTest {
    static int checks;
    static void check(boolean value, String message) { checks++; if (!value) throw new AssertionError(message); }
    static NetworkNavigationErrors fresh() {
        NetworkNavigationErrors state = new NetworkNavigationErrors();
        check(state.initialize("{\"frameTree\":{\"frame\":{\"id\":\"main\"}}}"), "frame tree bootstrap");
        return state;
    }
    static void request(NetworkNavigationErrors state, String id, String loader, String frame, String url) throws Exception {
        JSONObject value = new JSONObject().put("requestId", id).put("loaderId", loader).put("frameId", frame)
                .put("type", "Document").put("request", new JSONObject().put("url", url).put("method", "POST"));
        check(state.event("Network.requestWillBeSent", value.toString()) == null, "request is not terminal");
    }
    static NetworkNavigationErrors.Result fail(NetworkNavigationErrors state, String id, String error, boolean canceled) throws Exception {
        return state.event("Network.loadingFailed", new JSONObject().put("requestId", id)
                .put("errorText", error).put("canceled", canceled).toString());
    }
    static void response(NetworkNavigationErrors state, String id, String loader, String frame, int code) throws Exception {
        state.event("Network.responseReceived", new JSONObject().put("requestId", id).put("loaderId", loader)
                .put("frameId", frame).put("response", new JSONObject().put("status", code)).toString());
    }
    static void rejected(Runnable action) { try { action.run(); throw new AssertionError("Malformed identity accepted"); }
        catch (IllegalArgumentException expected) { checks++; } }
    public static void main(String[] args) throws Exception {
        String same = "https://download.invalid/same.apk";
        NetworkNavigationErrors state = fresh();
        // Observed runtime158 ordering: response200, CDP ERR_ABORTED/canceled, COM9,
        // DownloadStarting. A different request at that exact URL remains independent.
        request(state, "old", "loader1", "main", same); response(state, "old", "loader1", "main", 200);
        NetworkNavigationErrors.Result old = fail(state, "old", "net::ERR_ABORTED", true);
        check(old != null && old.mainFrame && old.canceled && old.url.equals(same), "download cancellation identity");
        request(state, "new", "loader2", "main", same);
        check(fail(state, "old", "net::ERR_ABORTED", true) == null, "late duplicate old request terminal");
        NetworkNavigationErrors.Result failed = fail(state, "new", "net::ERR_CONNECTION_ABORTED", false);
        check(failed != null && !failed.canceled && failed.mainFrame && failed.method.equals("POST")
                && failed.error.equals("net::ERR_CONNECTION_ABORTED"), "same URL genuine9 was masked");
        check(fail(state, "new", "net::ERR_EMPTY_RESPONSE", false) == null, "duplicate failed terminal");

        // A child download must not consume/convert a simultaneous main-frame failure.
        request(state, "child", "childLoader", "sub", same); request(state, "parent", "mainLoader", "main", same);
        check(fail(state, "child", "net::ERR_ABORTED", true) == null, "child frame entered main-frame observer");
        check(!fail(state, "parent", "net::ERR_CONNECTION_RESET", false).canceled, "child hid parent failure");
        request(state, "redirect", "redirectLoader", "main", "https://download.invalid/original");
        request(state, "redirect", "redirectLoader", "main", same);
        check(fail(state, "redirect", "net::ERR_ABORTED", false).url.equals(same), "redirect lost final identity");
        for (int code : new int[] {401, 407}) {
            request(state, "auth", "authLoader", "main", same); response(state, "auth", "authLoader", "main", code);
            check(fail(state, "auth", "net::ERR_INVALID_AUTH_CREDENTIALS", false) == null, "auth retry stolen");
            request(state, "retry", "retryLoader", "main", same); response(state, "retry", "retryLoader", "main", 200);
            check(fail(state, "retry", "net::ERR_ABORTED", true).canceled, "auth download retry lost");
        }
        request(state, "success", "successLoader", "main", same);
        state.event("Network.loadingFinished", "{\"requestId\":\"success\"}");
        check(fail(state, "success", "net::ERR_EMPTY_RESPONSE", false) == null, "completed loader resurrected");
        for (int code : new int[] {404, 500}) {
            request(state, "http", "httpLoader", "main", same); response(state, "http", "httpLoader", "main", code);
            check(fail(state, "http", "net::ERR_CONTENT_LENGTH_MISMATCH", false) == null,
                    "Native HTTP error became a second public network error");
        }
        request(state, "blocked", "blockedLoader", "main", same);
        NetworkNavigationErrors.Result blocked = state.event("Network.loadingFailed",
                "{\"requestId\":\"blocked\",\"errorText\":\"\",\"blockedReason\":\"csp\"}");
        check(blocked != null && !blocked.canceled && blocked.error.equals("net::ERR_BLOCKED_BY_RESPONSE"),
                "Legitimate CSP failure disabled network observation");
        request(state, "badloader", "correct", "main", same);
        final NetworkNavigationErrors current = state;
        rejected(() -> { try { response(current, "badloader", "other", "main", 200); } catch (RuntimeException e) { throw e; } catch(Exception e) { throw new AssertionError(e); } });
        rejected(() -> current.event("Network.requestWillBeSent", "{\"requestId\":\"bad\",\"type\":\"Document\"}"));
        state.clear();
        check(fail(state, "badloader", "net::ERR_EMPTY_RESPONSE", false) == null, "destroy retained pending request");
        check(!state.initialize("{\"frameTree\":{\"frame\":{\"id\":\"main\"}}}"), "destroyed observer reopened");
        NetworkNavigationErrors bootstrap = new NetworkNavigationErrors();
        check(!bootstrap.initialize("{}"), "missing main-frame bootstrap accepted");
        check(bootstrap.event("Network.loadingFailed", "{}") == null, "uninitialized observer emitted");
        NetworkNavigationErrors bounded = fresh();
        for (int i=0;i<NetworkNavigationErrors.MAX_REQUESTS;i++) request(bounded,"req"+i,"l"+i,"main",same);
        NetworkNavigationErrors.Result limited = bounded.event("Network.requestWillBeSent", new JSONObject()
                .put("requestId", "overflow").put("loaderId", "overflow").put("frameId", "main")
                .put("type", "Document").put("request", new JSONObject().put("url", same).put("method", "GET")).toString());
        check(limited != null && limited.limited && !limited.canceled && limited.url.equals(same)
                && limited.error.equals("net::ERR_INSUFFICIENT_RESOURCES"), "Identity pressure did not reject exactly the new request");
        NetworkNavigationErrors.Result repeatedPressure = bounded.event("Network.requestWillBeSent", new JSONObject()
                .put("requestId", "overflow2").put("loaderId", "overflow2").put("frameId", "main")
                .put("type", "Document").put("request", new JSONObject().put("url", same).put("method", "GET")).toString());
        check(repeatedPressure != null && repeatedPressure.limited && repeatedPressure.canceled,
                "Repeated identity pressure generated duplicate public errors");
        bounded.event("Network.loadingFinished", "{\"requestId\":\"req0\"}");
        request(bounded,"recovered","recoveryLoader","main",same);
        check(fail(bounded,"recovered","net::ERR_CONNECTION_RESET",false) != null,
                "Identity pressure permanently disabled later error observation");
        StringBuilder longUrl = new StringBuilder("https://long.invalid/");
        for (int i=0;i<17000;i++) longUrl.append('a');
        NetworkNavigationErrors longState = fresh();
        request(longState,"long","longLoader","main",longUrl.toString());
        check(fail(longState,"long","net::ERR_CONNECTION_RESET",false).url.equals(longUrl.toString()),
                "Long valid URL was truncated or rejected");
        bounded.clear();
        System.out.println("NetworkNavigationErrorsTest passed: " + checks + " checks");
    }
}
