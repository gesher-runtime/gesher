// Copyright (C) 2026 The Gesher Authors; All rights reserved.
package com.gesher.app.webview;

import java.util.HashMap;
import java.util.Map;
import org.json.JSONException;
import org.json.JSONObject;

/** Exact CDP document request identities. Never associates a download by its URL. */
final class NetworkNavigationErrors {
    static final int MAX_REQUESTS = 256;
    static final class Result {
        final String url, method, error;
        final boolean mainFrame, canceled, limited;
        Result(Request request, String error, boolean canceled) {
            this(request, error, canceled, false);
        }
        Result(Request request, String error, boolean canceled, boolean limited) {
            this.url = request.url;
            this.method = request.method;
            this.mainFrame = request.mainFrame;
            this.error = error;
            this.canceled = canceled;
            this.limited = limited;
        }
    }
    private static final class Request {
        String loader, frame, url, method;
        boolean mainFrame;
        int status;
    }
    private final Map<String, Request> requests = new HashMap<>();
    private String mainFrame;
    private boolean closed;
    private boolean pressure;

    synchronized boolean initialize(String tree) {
        if (closed) return false;
        try {
            mainFrame = required(new JSONObject(tree).getJSONObject("frameTree")
                    .getJSONObject("frame"), "id", 512);
            requests.clear();
            pressure = false;
            return true;
        } catch (JSONException | IllegalArgumentException e) {
            mainFrame = null;
            return false;
        }
    }

    synchronized Result event(String event, String json) {
        if (closed || mainFrame == null) return null;
        try {
            JSONObject value = new JSONObject(json);
            if (event.equals("Page.frameNavigated")) {
                JSONObject frame = value.getJSONObject("frame");
                if (!frame.has("parentId")) {
                    mainFrame = required(frame, "id", 512);
                    for (Request request : requests.values()) {
                        if (request.frame.equals(mainFrame)) request.mainFrame = true;
                    }
                }
                return null;
            }
            String id = required(value, "requestId", 512);
            if (event.equals("Network.requestWillBeSent")) {
                if (!"Document".equals(value.optString("type"))) return null;
                JSONObject metadata = value.getJSONObject("request");
                String url = required(metadata, "url", Integer.MAX_VALUE);
                if (!isHttp(url)) return null;
                String frame = required(value, "frameId", 512);
                if (!frame.equals(mainFrame)) return null;
                Request request = new Request();
                request.loader = required(value, "loaderId", 512);
                request.frame = frame;
                request.url = url;
                request.method = required(metadata, "method", 64);
                request.mainFrame = request.frame.equals(mainFrame);
                if (!requests.containsKey(id) && requests.size() >= MAX_REQUESTS) {
                    boolean notify = !pressure;
                    pressure = true;
                    return new Result(request, "net::ERR_INSUFFICIENT_RESOURCES", !notify, true);
                }
                requests.put(id, request);
                return null;
            }
            Request request = requests.get(id);
            if (request == null) return null;
            if (event.equals("Network.responseReceived")) {
                // The response must belong to this exact loader/frame, including redirects.
                if (!request.loader.equals(required(value, "loaderId", 512))
                        || !request.frame.equals(required(value, "frameId", 512))) {
                    throw new IllegalArgumentException("Document response identity mismatch");
                }
                request.status = value.getJSONObject("response").getInt("status");
                return null;
            }
            if (event.equals("Network.loadingFinished")) {
                requests.remove(id);
                pressure = false;
                return null;
            }
            if (!event.equals("Network.loadingFailed")) return null;
            requests.remove(id);
            pressure = false;
            // Basic/proxy auth is completed by the existing consent/retry machinery.
            if (request.status >= 400) return null;
            String error = value.getString("errorText");
            if (error.isEmpty()) error = "net::ERR_BLOCKED_BY_RESPONSE";
            boolean canceled = error.equals("net::ERR_ABORTED") || error.equals("ERR_ABORTED")
                    || value.optBoolean("canceled", false);
            return new Result(request, error, canceled);
        } catch (JSONException e) {
            throw new IllegalArgumentException("Malformed network identity event", e);
        }
    }

    synchronized void clear() {
        closed = true;
        mainFrame = null;
        requests.clear();
        pressure = false;
    }
    static boolean isHttp(String url) {
        return url != null && (url.startsWith("http://") || url.startsWith("https://"));
    }
    private static String required(JSONObject value, String key, int maximum) throws JSONException {
        String text = value.getString(key);
        if (text.isEmpty() || text.length() > maximum) throw new IllegalArgumentException("Invalid " + key);
        return text;
    }
}
