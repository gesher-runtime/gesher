// Copyright (C) 2026 The Gesher Authors; All rights reserved.
package com.gesher.app.webview;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** A private extension-worker channel. Page URLs never establish an auth owner. */
final class WebViewAuthBridge {
    static final String EXTENSION = "njmgmkjdhpmipobhjndpkbpegchdmpge";
    static final String WORKER = "chrome-extension://" + EXTENSION + "/worker.js";
    interface Transport {
        void command(String session, String method, String parameters, long token);
        void session(String session, String target);
        void ready(boolean available);
    }
    interface Scheduler {
        void post(Runnable work);
        void later(Runnable work, long milliseconds);
    }
    interface Prompt { void show(String url, String host, String realm, Reply reply); }
    interface Reply { void answer(boolean proceed, String user, String password); }
    private interface Completion { void done(JSONObject result) throws JSONException; }
    private static Broker broker;

    static final class Endpoint {
        final Transport transport;
        final Scheduler scheduler, coordinator;
        final Prompt prompt;
        final AtomicLong generation = new AtomicLong();
        final AtomicBoolean closed = new AtomicBoolean();
        final Map<String, Reply> replies = new ConcurrentHashMap<>();
        volatile Broker assigned;
        String target = "";
        boolean registered;
        Endpoint(Transport transport, Scheduler scheduler, Prompt prompt) {
            this(transport, scheduler, scheduler, prompt);
        }
        Endpoint(Transport transport, Scheduler scheduler, Scheduler coordinator, Prompt prompt) {
            this.transport = transport; this.scheduler = scheduler;
            this.coordinator = coordinator; this.prompt = prompt;
        }
        void installed(int status, String extensionId) {
            if (closed.get()) return;
            if (status < 0 || !EXTENSION.equals(extensionId)) { transport.ready(false); return; }
            Broker current;
            synchronized (WebViewAuthBridge.class) {
                if (broker == null) broker = new Broker(coordinator);
                current = broker; assigned = current;
            }
            current.scheduler.post(() -> { if (!closed.get()) current.add(this); });
        }
        void event(long token, int status, String method, String json, String session) {
            Broker current = assigned;
            if (current != null) current.scheduler.post(() -> {
                if (!closed.get()) current.receive(this, token, status, method, json, session);
            });
        }
        void stop() {
            generation.incrementAndGet(); replies.clear();
            Broker current = assigned;
            if (current != null) current.scheduler.post(() -> current.cancel(this, false));
        }
        void close() {
            if (!closed.compareAndSet(false, true)) return;
            generation.incrementAndGet(); replies.clear();
            Broker current = assigned;
            if (current != null) current.scheduler.post(() -> current.remove(this));
        }
        void unavailable() {
            generation.incrementAndGet(); replies.clear();
            Broker current = assigned;
            if (current != null) current.scheduler.post(current::unavailable);
        }
        void offer(JSONObject message) throws JSONException {
            String ticket = message.getString("ticket");
            if (!Long.toString(generation.get()).equals(message.optString("generation"))) {
                assigned.dispatch(object("op", "respond", "targetId", target, "ticket", ticket, "proceed", false));
                return;
            }
            if (replies.containsKey(ticket)) return;
            long issued = generation.get();
            AtomicBoolean answered = new AtomicBoolean();
            Broker current = assigned;
            Reply reply = (proceed, user, password) -> {
                if (!answered.compareAndSet(false, true) || closed.get()
                        || issued != generation.get() || replies.remove(ticket) == null) return;
                current.scheduler.post(() -> {
                    if (assigned != current || closed.get() || issued != generation.get()) return;
                    current.dispatch(object("op", "respond", "targetId", target,
                            "ticket", ticket, "proceed", proceed, "user", user == null ? "" : user,
                            "password", password == null ? "" : password));
                });
            };
            String url = message.getString("url"), host = message.optString("host"), realm = message.optString("realm");
            replies.put(ticket, reply);
            scheduler.post(() -> {
                if (closed.get() || generation.get() != issued || !replies.containsKey(ticket)) return;
                try { prompt.show(url, host, realm, reply); }
                catch (RuntimeException failure) { reply.answer(false, "", ""); }
            });
        }
    }

    private static final class Command {
        final Endpoint endpoint;
        final Completion completion;
        final long round;
        final boolean fenced;
        Command(Endpoint endpoint, Completion completion, long round, boolean fenced) {
            this.endpoint = endpoint; this.completion = completion;
            this.round = round; this.fenced = fenced;
        }
    }
    private static final class Broker {
        final Scheduler scheduler;
        final ArrayList<Endpoint> endpoints = new ArrayList<>();
        final Map<String, Endpoint> targets = new HashMap<>();
        final Map<Long, Command> commands = new HashMap<>();
        Endpoint owner;
        final String channel = UUID.randomUUID().toString();
        final String binding = "gesherAuth_" + channel.replace("-", "");
        String epoch = "", session = "", workerTarget = "";
        long token, round;
        int context;
        boolean ready, starting, waking;
        Broker(Scheduler scheduler) { this.scheduler = scheduler; }
        void add(Endpoint endpoint) {
            if (endpoint.closed.get() || endpoints.contains(endpoint)) return;
            endpoints.add(endpoint);
            command(endpoint, "", "Target.getTargetInfo", object(), result -> {
                String id = result.getJSONObject("targetInfo").getString("targetId");
                if (id.isEmpty() || targets.containsKey(id)) throw new JSONException("Invalid target identity");
                endpoint.target = id;
                targets.put(id, endpoint);
                if (ready) register(endpoint); else if (!starting) start();
            });
        }
        void command(Endpoint endpoint, String sessionId, String method, JSONObject params, Completion callback) {
            if (endpoint == null || endpoint.closed.get()) return;
            if (commands.size() >= 256) { unavailable(); return; }
            long id = ++token;
            commands.put(id, new Command(endpoint, callback, round, !method.equals("Target.getTargetInfo")));
            endpoint.transport.command(sessionId, method, params.toString(), id);
        }
        void start() {
            owner = endpoints.stream().filter(value -> !value.closed.get() && !value.target.isEmpty())
                    .findFirst().orElse(null);
            if (owner == null) return;
            starting = true; ready = false;
            waking = false;
            epoch = UUID.randomUUID().toString(); session = ""; workerTarget = ""; context = 0;
            long attempt = ++round;
            scheduler.later(() -> { if (attempt == round && !ready) unavailable(); }, 10000);
            command(owner, "", "Target.setDiscoverTargets", object("discover", true), result -> discover(attempt));
        }
        void discover(long attempt) {
            if (attempt != round || owner == null || owner.closed.get()) return;
            command(owner, "", "Target.getTargets", object(), result -> {
                JSONArray list = result.getJSONArray("targetInfos");
                String id = "";
                for (int i = 0; i < list.length(); i++) {
                    JSONObject info = list.getJSONObject(i);
                    if (WORKER.equals(info.optString("url")) && "service_worker".equals(info.optString("type"))) {
                        id = info.getString("targetId"); break;
                    }
                }
                if (id.isEmpty()) {
                    if (!waking) {
                        waking = true;
                        command(owner, "", "ServiceWorker.enable", object(), enabled ->
                                command(owner, "", "ServiceWorker.startWorker",
                                        object("scopeURL", "chrome-extension://" + EXTENSION + "/"), woke ->
                                            scheduler.later(() -> discover(attempt), 100)));
                        return;
                    }
                    scheduler.later(() -> discover(attempt), 100);
                    return;
                }
                workerTarget = id;
                command(owner, "", "Target.attachToTarget", object("targetId", id, "flatten", true), attached -> {
                    session = attached.getString("sessionId");
                    owner.transport.session(session, workerTarget);
                    command(owner, session, "Runtime.enable", object(), enabled -> bind());
                });
            });
        }
        void hello() {
            if (starting) dispatch(object("op", "hello", "binding", binding, "bootTarget", owner.target));
        }
        void bind() {
            if (!starting || owner == null) return;
            if (context == 0) { scheduler.later(this::bind, 100); return; }
            command(owner, session, "Runtime.addBinding", object("name", binding,
                    "executionContextId", context), bound -> hello());
        }
        void dispatch(JSONObject message) {
            if ((!starting && !ready) || owner == null || owner.closed.get() || session.isEmpty()) return;
            try { message.put("epoch", epoch); message.put("channel", channel); } catch (JSONException impossible) { throw new AssertionError(impossible); }
            command(owner, session, "Runtime.evaluate", object("expression",
                    "globalThis.gesherAuthDispatch(" + message + ")", "returnByValue", true,
                    "contextId", context), result -> {
                if (result.has("exceptionDetails")) {
                    if (starting) scheduler.later(this::hello, 100); else unavailable();
                }
            });
        }
        void register(Endpoint endpoint) {
            if (!endpoint.target.isEmpty() && !endpoint.closed.get()) {
                dispatch(object("op", "register", "targetId", endpoint.target, "generation", Long.toString(endpoint.generation.get())));
            }
        }
        void receive(Endpoint endpoint, long id, int status, String method, String json, String eventSession) {
            try {
                if (id != 0) {
                    Command pending = commands.remove(id);
                    if (pending == null || pending.endpoint != endpoint || endpoint.closed.get()
                            || pending.fenced && pending.round != round) return;
                    if (status < 0 && !method.equals("ServiceWorker.startWorker")) { unavailable(); return; }
                    pending.completion.done(status < 0 ? object() : new JSONObject(json));
                    return;
                }
                if (endpoint != owner || (!ready && !starting)) return;
                JSONObject event = new JSONObject(json);
                if (method.equals("Runtime.executionContextCreated") && session.equals(eventSession)) {
                    JSONObject info = event.getJSONObject("context");
                    String origin = info.optString("origin");
                    JSONObject auxiliary = info.optJSONObject("auxData");
                    if ((origin.equals(WORKER) || origin.equals("chrome-extension://" + EXTENSION)
                            || origin.equals("chrome-extension://" + EXTENSION + "/"))
                            && (auxiliary == null || auxiliary.optBoolean("isDefault", false))) {
                        context = info.getInt("id");
                    }
                    return;
                }
                if ((method.equals("Runtime.executionContextsCleared")
                        || method.equals("Runtime.executionContextDestroyed")
                        && event.optInt("executionContextId") == context) && session.equals(eventSession)) {
                    restart(); return;
                }
                if (method.equals("Target.targetDestroyed") || method.equals("Target.detachedFromTarget")) {
                    if (workerTarget.equals(event.optString("targetId")) || session.equals(event.optString("sessionId"))) restart();
                    return;
                }
                if (!method.equals("Runtime.bindingCalled") || !session.equals(eventSession)
                        || !binding.equals(event.optString("name"))
                        || context == 0 || event.optInt("executionContextId") != context) return;
                JSONObject message = new JSONObject(event.getString("payload"));
                if (message.optInt("version") != 1 || !epoch.equals(message.optString("epoch"))
                        || !channel.equals(message.optString("channel"))) return;
                String kind = message.getString("event");
                if (kind.equals("ready")) {
                    ready = true; starting = false;
                    for (Endpoint view : endpoints) register(view);
                    return;
                }
                if (kind.equals("unavailable")) { unavailable(); return; }
                Endpoint view = targets.get(message.optString("targetId"));
                if (view == null || view.closed.get()) return;
                if (kind.equals("registered")) {
                    view.registered = true; view.transport.ready(true);
                } else if (kind.equals("registrationFailed")) {
                    view.transport.ready(false);
                } else if (kind.equals("auth") && view.registered) {
                    view.offer(message);
                } else if (kind.equals("ended")) view.replies.remove(message.optString("ticket"));
            } catch (JSONException | RuntimeException failure) {
                // Never log raw protocol/authentication data or parse exception text.
                unavailable();
            }
        }
        void cancel(Endpoint endpoint, boolean remove) {
            if (!endpoint.target.isEmpty()) dispatch(object("op", remove ? "unregister" : "cancelTarget",
                    "targetId", endpoint.target, "generation", Long.toString(endpoint.generation.get())));
        }
        void remove(Endpoint endpoint) {
            cancel(endpoint, true);
            endpoints.remove(endpoint);
            targets.remove(endpoint.target);
            commands.entrySet().removeIf(entry -> entry.getValue().endpoint == endpoint);
            if (owner == endpoint) restart();
            if (endpoints.isEmpty()) {
                ++round; commands.clear();
                synchronized (WebViewAuthBridge.class) { if (broker == this) broker = null; }
            }
        }
        void restart() {
            retire();
            ++round; ready = false; starting = false;
            commands.entrySet().removeIf(entry -> entry.getValue().fenced || entry.getValue().endpoint.closed.get());
            for (Endpoint endpoint : endpoints) {
                endpoint.registered = false; endpoint.generation.incrementAndGet(); endpoint.replies.clear();
            }
            start();
        }
        void unavailable() {
            if (!starting && !ready) return;
            retire();
            ++round; starting = false; ready = false; commands.clear();
            for (Endpoint endpoint : endpoints) {
                endpoint.generation.incrementAndGet(); endpoint.replies.clear(); endpoint.registered = false;
                endpoint.transport.ready(false);
            }
        }
        void retire() {
            if (owner != null && !owner.closed.get() && !session.isEmpty()) {
                JSONObject shutdown = object("op", "shutdown", "epoch", epoch, "channel", channel);
                owner.transport.command(session, "Runtime.evaluate", object("expression",
                        "globalThis.gesherAuthDispatch(" + shutdown + ")", "contextId", context).toString(), 0);
                owner.transport.command(session, "Runtime.removeBinding", object("name", binding).toString(), 0);
                owner.transport.command("", "Target.detachFromTarget", object("sessionId", session).toString(), 0);
                owner.transport.session(session, "");
            }
        }
    }
    private static JSONObject object(Object... values) {
        JSONObject result = new JSONObject();
        try { for (int i = 0; i < values.length; i += 2) result.put((String) values[i], values[i + 1]); }
        catch (JSONException impossible) { throw new AssertionError(impossible); }
        return result;
    }
}
