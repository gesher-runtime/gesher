/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.IServiceConnection;
import android.content.AttributionSource;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.ServiceInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.speech.IModelDownloadListener;
import android.speech.IRecognitionListener;
import android.speech.IRecognitionService;
import android.speech.IRecognitionServiceManagerCallback;
import android.speech.IRecognitionSupportCallback;
import android.speech.RecognitionSupport;
import android.speech.RecognizerIntent;
import android.speech.SpeechRecognizer;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/** Drives the production broker with installed-service and process boundaries under fixture control. */
public final class SpeechRecognitionContractTest {
    private static int checks;
    private static final ComponentName SERVICE = new ComponentName("fixture.speech", "fixture.speech.Engine");
    private static final ComponentName OTHER = new ComponentName("fixture.other", "fixture.other.Other");
    private static final Intent INTENT = new Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH)
            .putExtra(RecognizerIntent.EXTRA_LANGUAGE, "he-IL")
            .putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, true);

    private static void check(boolean value, String description) {
        if (!value) throw new AssertionError(description);
        checks++;
    }

    private static <T> T take(BlockingQueue<T> queue) throws InterruptedException {
        T value = queue.poll(4, TimeUnit.SECONDS);
        if (value == null) throw new AssertionError("Timed out awaiting test event");
        return value;
    }

    private static final class Lifetime extends Binder {
        private final List<IBinder.DeathRecipient> recipients = new ArrayList<>();
        @Override public synchronized void linkToDeath(IBinder.DeathRecipient recipient, int flags) {
            recipients.add(recipient);
        }
        @Override public synchronized boolean unlinkToDeath(IBinder.DeathRecipient recipient, int flags) {
            return recipients.remove(recipient);
        }
        synchronized void die() {
            for (IBinder.DeathRecipient recipient : List.copyOf(recipients)) recipient.binderDied();
            recipients.clear();
        }
        synchronized boolean detached() { return recipients.isEmpty(); }
        synchronized int links() { return recipients.size(); }
    }

    private static class Backend implements SpeechRecognitionManagerService.Backend {
        final Lifetime process = new Lifetime();
        final SpeechRecognitionManagerService.Owner client = new SpeechRecognitionManagerService.Owner(
                7210, 10134, "fixture.client", new Object(), process);
        final SpeechRecognitionManagerService.Provider provider =
                new SpeechRecognitionManagerService.Provider(SERVICE, new Object());
        final BlockingQueue<IServiceConnection> bindings = new LinkedBlockingQueue<>();
        final BlockingQueue<IServiceConnection> unbindings = new LinkedBlockingQueue<>();
        volatile SpeechRecognitionManagerService.Owner caller = client;
        volatile boolean enabled = true;
        boolean rejectBinding;

        @Override public SpeechRecognitionManagerService.Owner caller() { return caller; }
        @Override public boolean isCurrent(SpeechRecognitionManagerService.Owner owner) {
            return client.equals(owner);
        }
        @Override public boolean canManage(SpeechRecognitionManagerService.Owner owner) { return false; }
        @Override public AttributionSource authenticate(SpeechRecognitionManagerService.Owner owner,
                AttributionSource source) {
            if (owner.uid() != source.getUid() || !owner.packageName().equals(source.getPackageName())) {
                throw new SecurityException("Fixture rejected attribution");
            }
            return source;
        }
        @Override public ComponentName onDeviceComponent() { return null; }
        @Override public SpeechRecognitionManagerService.Provider resolve(ComponentName component) {
            return enabled && SERVICE.equals(component) ? provider : null;
        }
        @Override public boolean isCurrent(SpeechRecognitionManagerService.Provider selected) {
            return enabled && provider.equals(selected);
        }
        @Override public boolean bind(SpeechRecognitionManagerService.Provider selected, IServiceConnection link) {
            bindings.add(link);
            return !rejectBinding;
        }
        @Override public void unbind(IServiceConnection link) { unbindings.add(link); }

        AttributionSource attribution() {
            return new AttributionSource.Builder(client.uid()).setPackageName(client.packageName()).build();
        }
    }

    private static final class QuotaBackend extends Backend {
        final Lifetime otherProcess = new Lifetime();
        final Lifetime sharedUidProcess = new Lifetime();
        final SpeechRecognitionManagerService.Owner otherClient = new SpeechRecognitionManagerService.Owner(
                7211, 10135, "fixture.second", new Object(), otherProcess);
        final SpeechRecognitionManagerService.Owner sharedUidClient = new SpeechRecognitionManagerService.Owner(
                7212, client.uid(), "fixture.shared", new Object(), sharedUidProcess);
        final Map<ComponentName, SpeechRecognitionManagerService.Provider> providers = new HashMap<>();

        QuotaBackend() {
            for (int i = 0; i < 11; i++) {
                ComponentName component = component(i);
                providers.put(component, new SpeechRecognitionManagerService.Provider(component, new Object()));
            }
        }

        ComponentName component(int index) {
            return new ComponentName("fixture.speech" + index, "fixture.speech" + index + ".Engine");
        }

        @Override public boolean isCurrent(SpeechRecognitionManagerService.Owner owner) {
            return client.equals(owner) || otherClient.equals(owner) || sharedUidClient.equals(owner);
        }
        @Override public SpeechRecognitionManagerService.Provider resolve(ComponentName component) {
            return enabled ? providers.get(component) : null;
        }
        @Override public boolean isCurrent(SpeechRecognitionManagerService.Provider selected) {
            return enabled && providers.get(selected.component()) == selected;
        }
    }

    private static class Admission extends IRecognitionServiceManagerCallback.Stub {
        final BlockingQueue<Object> events = new LinkedBlockingQueue<>();
        @Override public void onSuccess(IRecognitionService service) { events.add(service); }
        @Override public void onError(int code) { events.add(code); }
    }

    private record Listening(Intent intent, IRecognitionListener listener, AttributionSource source) {}

    private static final class Engine extends IRecognitionService.Stub {
        final BlockingQueue<Listening> starts = new LinkedBlockingQueue<>();
        final BlockingQueue<IRecognitionListener> stops = new LinkedBlockingQueue<>();
        final BlockingQueue<IRecognitionListener> cancels = new LinkedBlockingQueue<>();
        final BlockingQueue<IRecognitionSupportCallback> support = new LinkedBlockingQueue<>();
        final BlockingQueue<IModelDownloadListener> downloads = new LinkedBlockingQueue<>();
        final BlockingQueue<Intent> downloadsWithoutCallback = new LinkedBlockingQueue<>();

        @Override public void startListening(Intent intent, IRecognitionListener listener,
                AttributionSource source) { starts.add(new Listening(intent, listener, source)); }
        @Override public void stopListening(IRecognitionListener listener) { stops.add(listener); }
        @Override public void cancel(IRecognitionListener listener, boolean shutdown) { cancels.add(listener); }
        @Override public void checkRecognitionSupport(Intent intent, AttributionSource source,
                IRecognitionSupportCallback callback) { support.add(callback); }
        @Override public void triggerModelDownload(Intent intent, AttributionSource source,
                IModelDownloadListener callback) {
            if (callback == null) downloadsWithoutCallback.add(intent);
            else downloads.add(callback);
        }
    }

    private static final class Listener extends IRecognitionListener.Stub {
        final BlockingQueue<Object> events = new LinkedBlockingQueue<>();
        @Override public void onReadyForSpeech(Bundle value) { events.add("ready"); }
        @Override public void onBeginningOfSpeech() { events.add("start"); }
        @Override public void onRmsChanged(float value) { events.add(value); }
        @Override public void onBufferReceived(byte[] value) { events.add(value); }
        @Override public void onEndOfSpeech() { events.add("end"); }
        @Override public void onError(int value) { events.add(value); }
        @Override public void onResults(Bundle value) { events.add(value); }
        @Override public void onPartialResults(Bundle value) { events.add(value); }
        @Override public void onSegmentResults(Bundle value) { events.add(value); }
        @Override public void onEndOfSegmentedSession() { events.add("end-segment"); }
        @Override public void onLanguageDetection(Bundle value) { events.add(value); }
        @Override public void onEvent(int value, Bundle params) { events.add(value); }
    }

    private static final class Support extends IRecognitionSupportCallback.Stub {
        final BlockingQueue<Object> events = new LinkedBlockingQueue<>();
        @Override public void onSupportResult(RecognitionSupport result) { events.add(result); }
        @Override public void onError(int code) { events.add(code); }
    }

    private static final class Model extends IModelDownloadListener.Stub {
        final BlockingQueue<Object> events = new LinkedBlockingQueue<>();
        @Override public void onProgress(int percent) { events.add(percent); }
        @Override public void onSuccess() { events.add("loaded"); }
        @Override public void onScheduled() { events.add("scheduled"); }
        @Override public void onError(int code) { events.add(code); }
    }

    private static void reject(Throwing action, String description) throws Exception {
        try { action.run(); }
        catch (SecurityException expected) { checks++; return; }
        throw new AssertionError(description);
    }

    @FunctionalInterface private interface Throwing { void run() throws Exception; }

    private static void lifecycle() throws Exception {
        Backend backend = new Backend();
        SpeechRecognitionManagerService manager = new SpeechRecognitionManagerService(backend, 2000);
        Lifetime token = new Lifetime();
        Admission first = new Admission();
        Admission duplicate = new Admission();
        manager.createSession(SERVICE, token, false, first);
        IServiceConnection link = take(backend.bindings);
        manager.createSession(SERVICE, token, false, duplicate);
        Engine engine = new Engine();
        link.connected(SERVICE, engine, null, false);
        IRecognitionService client = (IRecognitionService) take(first.events);
        check(take(duplicate.events) == client && backend.bindings.isEmpty(), "one binding per client token");

        Listener listener = new Listener();
        AttributionSource attribution = backend.attribution();
        backend.caller = new SpeechRecognitionManagerService.Owner(7211, backend.client.uid(),
                "fixture.other", new Object(), null);
        reject(() -> client.startListening(INTENT, listener, attribution), "shared UID cannot transfer a session");
        reject(() -> client.cancel(listener, true), "foreign destruction");
        backend.caller = backend.client;
        reject(() -> client.startListening(INTENT, listener,
                new AttributionSource.Builder(backend.client.uid()).setPackageName("another").build()),
                "cannot submit another package attribution");
        reject(() -> manager.setTemporaryComponent(SERVICE), "temporary recognition is privileged");

        client.startListening(INTENT, listener, attribution);
        Listening firstAttempt = take(engine.starts);
        check(firstAttempt.intent.getStringExtra(RecognizerIntent.EXTRA_LANGUAGE).equals("he-IL")
                && firstAttempt.intent.getBooleanExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, false),
                "recognizer request inputs are forwarded unchanged");
        check(firstAttempt.source == attribution, "verified attribution is relayed");
        firstAttempt.listener.onReadyForSpeech(Bundle.EMPTY);
        check(take(listener.events).equals("ready"), "provider readiness reaches client");
        client.startListening(INTENT, listener, attribution);
        check(take(listener.events).equals(SpeechRecognizer.ERROR_CLIENT) && engine.starts.isEmpty(),
                "duplicate start cannot replace active listener");
        client.stopListening(listener);
        check(take(engine.stops) == firstAttempt.listener, "stop routes to original provider attempt");
        Bundle finalResult = new Bundle();
        finalResult.putStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION,
                new ArrayList<>(List.of("fixture result")));
        firstAttempt.listener.onResults(finalResult);
        check(take(listener.events) == finalResult, "results are untouched");
        firstAttempt.listener.onError(SpeechRecognizer.ERROR_SERVER);

        client.startListening(INTENT, listener, attribution);
        Listening second = take(engine.starts);
        check(second.listener != firstAttempt.listener, "each utterance has its own callback Binder");
        byte[] buffer = {1, 2, 3};
        second.listener.onBufferReceived(buffer);
        check(take(listener.events) == buffer, "stale error discarded and buffer unchanged");
        client.cancel(listener, false);
        check(take(engine.cancels) == second.listener, "cancel reaches matching provider attempt");
        second.listener.onResults(finalResult);
        client.startListening(INTENT, listener, attribution);
        Listening third = take(engine.starts);
        third.listener.onEndOfSegmentedSession();
        check(take(listener.events).equals("end-segment"), "segmented terminal wins after silent cancel");
        client.cancel(listener, true);
        check(take(backend.unbindings) == link && token.detached() && backend.process.detached(),
                "destroy releases connection and death links");
    }

    private static void supportAndDeath() throws Exception {
        Backend backend = new Backend();
        SpeechRecognitionManagerService manager = new SpeechRecognitionManagerService(backend, 2000);
        Lifetime token = new Lifetime();
        Admission callback = new Admission();
        manager.createSession(SERVICE, token, false, callback);
        IServiceConnection link = take(backend.bindings);
        Engine engine = new Engine();
        link.connected(SERVICE, engine, null, false);
        IRecognitionService client = (IRecognitionService) take(callback.events);
        AttributionSource attribution = backend.attribution();

        Support support = new Support();
        client.checkRecognitionSupport(INTENT, attribution, support);
        IRecognitionSupportCallback providerSupport = take(engine.support);
        RecognitionSupport installed = new RecognitionSupport.Builder()
                .setInstalledOnDeviceLanguages(List.of("he-IL")).build();
        providerSupport.onSupportResult(installed);
        providerSupport.onError(SpeechRecognizer.ERROR_SERVER);
        check(take(support.events) == installed, "provider owns model/language support result");

        Model model = new Model();
        client.triggerModelDownload(INTENT, attribution, model);
        IModelDownloadListener download = take(engine.downloads);
        download.onProgress(42);
        download.onScheduled();
        download.onSuccess();
        check(take(model.events).equals(42) && take(model.events).equals("scheduled"),
                "download progress and scheduling relayed");
        client.triggerModelDownload(INTENT, attribution, null);
        check(take(engine.downloadsWithoutCallback) == INTENT, "legacy download without listener is forwarded");

        Listener listener = new Listener();
        client.startListening(INTENT, listener, attribution);
        Listening started = take(engine.starts);
        client.checkRecognitionSupport(INTENT, attribution, support);
        IRecognitionSupportCallback lostSupport = take(engine.support);
        client.triggerModelDownload(INTENT, attribution, model);
        IModelDownloadListener lostDownload = take(engine.downloads);
        link.connected(SERVICE, null, null, true);
        int disconnected = SpeechRecognizer.ERROR_SERVER_DISCONNECTED;
        check(take(backend.unbindings) == link, "disconnect detaches the lost provider");
        check(take(listener.events).equals(disconnected) && take(support.events).equals(disconnected)
                && take(model.events).equals(disconnected), "disconnect reports each pending operation once");
        check(engine.cancels.isEmpty(), "disconnected provider is not called while retiring active listening");
        lostSupport.onSupportResult(installed);
        lostDownload.onSuccess();
        started.listener.onBeginningOfSpeech();

        client.startListening(INTENT, listener, attribution);
        IServiceConnection rebound = take(backend.bindings);
        check(rebound != link, "next operation can reconnect after provider death");
        Engine replacement = new Engine();
        rebound.connected(SERVICE, replacement, null, false);
        Listening active = take(replacement.starts);
        active.listener.onReadyForSpeech(Bundle.EMPTY);
        check(take(listener.events).equals("ready") && support.events.isEmpty() && model.events.isEmpty(),
                "reconnect suppresses old provider callbacks");
        backend.process.die();
        check(take(replacement.cancels) == active.listener && take(backend.unbindings) == rebound && token.detached(),
                "actual owning process death cancels even with its session token still live");
    }

    private static void unavailableAndShared() throws Exception {
        Backend backend = new Backend();
        SpeechRecognitionManagerService manager = new SpeechRecognitionManagerService(backend, 80);
        Admission callback = new Admission();
        Lifetime bindingToken = new Lifetime();
        manager.createSession(null, bindingToken, false, callback);
        check(take(callback.events).equals(SpeechRecognizer.ERROR_CLIENT), "no selected provider");
        manager.createSession(SERVICE, bindingToken, true, callback);
        check(take(callback.events).equals(SpeechRecognizer.ERROR_CLIENT), "no configured on-device provider");
        manager.createSession(OTHER, bindingToken, false, callback);
        check(take(callback.events).equals(SpeechRecognizer.ERROR_TOO_MANY_REQUESTS), "reject unrelated component");
        backend.rejectBinding = true;
        manager.createSession(SERVICE, new Lifetime(), false, callback);
        check(take(callback.events).equals(SpeechRecognizer.ERROR_CLIENT)
                && take(backend.unbindings) == take(backend.bindings), "initial bind failure unwinds");
        backend.rejectBinding = false;
        Admission timeout = new Admission() {
            @Override public void onError(int code) {
                events.add(bindingToken.detached() && backend.process.detached()
                        ? code : "failed session still linked");
            }
        };
        manager.createSession(SERVICE, bindingToken, false, timeout);
        IServiceConnection timed = take(backend.bindings);
        check(take(timeout.events).equals(SpeechRecognizer.ERROR_CLIENT),
                "failed admission releases its death links before reporting the error");
        check(take(backend.unbindings) == timed, "bind timeout releases service binding");
        timed.connected(SERVICE, new Engine(), null, false);

        manager.createSession(SERVICE, bindingToken, false, callback);
        IServiceConnection newLink = take(backend.bindings);
        newLink.connected(SERVICE, new Engine(), null, false);
        IRecognitionService client = (IRecognitionService) take(callback.events);
        check(client != null && callback.events.isEmpty(), "late old publication cannot complete a new session");
        client.cancel(new Listener(), true);
        check(take(backend.unbindings) == newLink, "terminal cleanup after timeout retry");

        SpeechRecognitionManagerService shared = new SpeechRecognitionManagerService(backend, 2000);
        List<Admission> admissions = new ArrayList<>();
        for (int i = 0; i < 11; i++) {
            Admission next = new Admission();
            admissions.add(next);
            shared.createSession(SERVICE, new Lifetime(), false, next);
        }
        IServiceConnection sharedLink = take(backend.bindings);
        check(backend.bindings.isEmpty(), "eleven sessions for one UID/provider share one remote connection");
        Engine sharedEngine = new Engine();
        sharedLink.connected(SERVICE, sharedEngine, null, false);
        List<IRecognitionService> sessions = new ArrayList<>();
        for (Admission next : admissions) {
            sessions.add((IRecognitionService) take(next.events));
        }
        check(sessions.size() == 11, "all eleven same-provider session tokens are independently admitted");
        for (IRecognitionService session : sessions) session.cancel(null, true);
        check(take(backend.unbindings) == sharedLink && backend.unbindings.isEmpty(),
                "shared provider connection is released after the final client session");

        SpeechRecognitionManagerService fanout = new SpeechRecognitionManagerService(backend, 2000);
        Admission fanoutAdmission = new Admission();
        fanout.createSession(SERVICE, new Lifetime(), false, fanoutAdmission);
        IServiceConnection fanoutLink = take(backend.bindings);
        Engine fanoutEngine = new Engine();
        fanoutLink.connected(SERVICE, fanoutEngine, null, false);
        IRecognitionService fanoutClient = (IRecognitionService) take(fanoutAdmission.events);
        for (int i = 0; i < 16; i++) {
            IModelDownloadListener request = new IModelDownloadListener.Stub() {
                @Override public void onProgress(int percent) {}
                @Override public void onSuccess() {}
                @Override public void onScheduled() {}
                @Override public void onError(int code) {}
            };
            fanoutClient.triggerModelDownload(INTENT, backend.attribution(), request);
        }
        List<IModelDownloadListener> downloads = new ArrayList<>();
        for (int i = 0; i < 16; i++) downloads.add(take(fanoutEngine.downloads));
        IRecognitionSupportCallback supportRequest = new IRecognitionSupportCallback.Stub() {
            @Override public void onSupportResult(RecognitionSupport result) {}
            @Override public void onError(int code) {}
        };
        fanoutClient.checkRecognitionSupport(INTENT, backend.attribution(), supportRequest);
        IRecognitionSupportCallback forwardedSupport = take(fanoutEngine.support);
        check(downloads.size() == 16 && forwardedSupport != null,
                "support query reaches provider while sixteen model-download callbacks remain pending");
        for (IModelDownloadListener download : downloads) download.onError(SpeechRecognizer.ERROR_CLIENT);
        forwardedSupport.onError(SpeechRecognizer.ERROR_CLIENT);
        fanoutClient.cancel(null, true);
        check(take(backend.unbindings) == fanoutLink, "request fanout session releases its provider connection");
    }

    private static void manifestEligibility() {
        ServiceInfo service = new ServiceInfo();
        service.applicationInfo = new ApplicationInfo();
        service.enabled = true;
        service.applicationInfo.enabled = true;
        check(!SpeechRecognitionManagerService.isProvider(service), "hidden component cannot be a cross-app engine");
        service.exported = true;
        check(SpeechRecognitionManagerService.isProvider(service), "valid provider needs no nonstandard permission");
        service.applicationInfo.enabled = false;
        check(!SpeechRecognitionManagerService.isProvider(service), "disabled package cannot be an engine");
    }

    private static void connectionQuota() throws Exception {
        QuotaBackend backend = new QuotaBackend();
        SpeechRecognitionManagerService manager = new SpeechRecognitionManagerService(backend, 2000);
        List<Lifetime> tokens = new ArrayList<>();
        List<IRecognitionService> clients = new ArrayList<>();
        List<IServiceConnection> links = new ArrayList<>();
        Admission pending = new Admission();
        for (int i = 0; i < 10; i++) {
            Lifetime token = new Lifetime();
            Admission admission = i == 9 ? pending : new Admission();
            tokens.add(token);
            manager.createSession(backend.component(i), token, false, admission);
            IServiceConnection link = take(backend.bindings);
            links.add(link);
            if (i == 9) clients.add(null);
            else {
                link.connected(backend.component(i), new Engine(), null, false);
                clients.add((IRecognitionService) take(admission.events));
            }
        }
        check(backend.process.links() == 10, "connected and pending providers consume the same UID quota");

        Lifetime deniedToken = new Lifetime();
        Admission denied = new Admission();
        manager.createSession(backend.component(10), deniedToken, false, denied);
        // An already admitted token is a worker barrier, without creating a
        // connection or depending on an arbitrary delay to observe rejection.
        Admission barrier = new Admission();
        manager.createSession(backend.component(0), tokens.get(0), false, barrier);
        check(take(barrier.events) == clients.get(0), "existing admission remains usable at the connection limit");
        check(backend.bindings.isEmpty(), "eleventh distinct provider never starts a binding");
        check(take(denied.events).equals(SpeechRecognizer.ERROR_TOO_MANY_REQUESTS)
                && deniedToken.detached() && backend.process.links() == 10,
                "quota rejection leaves no session or process death link");

        Lifetime saturatedToken = new Lifetime();
        manager.createSession(backend.component(0), saturatedToken, false, denied);
        check(take(denied.events).equals(SpeechRecognizer.ERROR_TOO_MANY_REQUESTS)
                && saturatedToken.detached() && backend.bindings.isEmpty(),
                "Android checks saturated UID connections before new same-provider admission");

        backend.caller = backend.sharedUidClient;
        Lifetime sharedUidToken = new Lifetime();
        manager.createSession(backend.component(10), sharedUidToken, false, denied);
        check(take(denied.events).equals(SpeechRecognizer.ERROR_TOO_MANY_REQUESTS)
                && sharedUidToken.detached() && backend.sharedUidProcess.detached(),
                "another package and process with the same UID share its connection quota");

        links.get(9).connected(backend.component(9), new Engine(), null, false);
        clients.set(9, (IRecognitionService) take(pending.events));
        backend.caller = backend.otherClient;
        Lifetime otherToken = new Lifetime();
        Admission otherAdmission = new Admission();
        manager.createSession(backend.component(0), otherToken, false, otherAdmission);
        IServiceConnection otherLink = take(backend.bindings);
        otherLink.connected(backend.component(0), new Engine(), null, false);
        IRecognitionService otherClient = (IRecognitionService) take(otherAdmission.events);
        check(backend.otherProcess.links() == 1 && backend.process.links() == 10,
                "another UID can bind while the first UID has ten providers");
        otherClient.cancel(null, true);
        check(take(backend.unbindings) == otherLink && otherToken.detached()
                && backend.otherProcess.detached(), "other UID cleanup preserves the first UID's quota");
        backend.caller = backend.client;

        clients.get(0).cancel(null, true);
        check(take(backend.unbindings) == links.get(0) && tokens.get(0).detached(),
                "last session release frees one distinct connection");
        manager.createSession(backend.component(10), deniedToken, false, denied);
        IServiceConnection retry = take(backend.bindings);
        retry.connected(backend.component(10), new Engine(), null, false);
        clients.set(0, (IRecognitionService) take(denied.events));
        check(!deniedToken.detached() && backend.process.links() == 10,
                "same previously rejected token can retry after a connection is freed");

        clients.get(1).cancel(null, true);
        check(take(backend.unbindings) == links.get(1), "release opens a slot for bind-failure control");
        Lifetime failedToken = new Lifetime();
        Admission failed = new Admission();
        backend.rejectBinding = true;
        manager.createSession(backend.component(1), failedToken, false, failed);
        IServiceConnection failedLink = take(backend.bindings);
        check(take(failed.events).equals(SpeechRecognizer.ERROR_CLIENT)
                && take(backend.unbindings) == failedLink && failedToken.detached()
                && backend.process.links() == 9, "failed bind releases quota and death-link ownership");
        backend.rejectBinding = false;
        manager.createSession(backend.component(1), failedToken, false, failed);
        IServiceConnection recovered = take(backend.bindings);
        recovered.connected(backend.component(1), new Engine(), null, false);
        clients.set(1, (IRecognitionService) take(failed.events));
        check(backend.process.links() == 10, "bind failure permits a fresh attempt with the same token");

        // The logical remote-service reservation survives a transient provider
        // loss with its delivered endpoint, as in Android's per-UID service set.
        links.get(2).connected(backend.component(2), null, null, true);
        check(take(backend.unbindings) == links.get(2), "provider loss releases its dead binding");
        Lifetime replacementToken = new Lifetime();
        Admission replacementAdmission = new Admission();
        manager.createSession(backend.component(0), replacementToken, false, replacementAdmission);
        manager.createSession(backend.component(10), deniedToken, false, barrier);
        check(take(barrier.events) == clients.get(0), "existing token synchronizes the disconnect admission check");
        check(backend.bindings.isEmpty(), "provider loss retains the admitted endpoint's quota reservation");
        check(take(replacementAdmission.events).equals(SpeechRecognizer.ERROR_TOO_MANY_REQUESTS)
                && replacementToken.detached() && backend.process.links() == 10,
                "provider loss cannot admit an eleventh distinct service or leak rejected-token ownership");

        Listener listening = new Listener();
        clients.get(2).startListening(INTENT, listening, backend.attribution());
        IServiceConnection reconnected = take(backend.bindings);
        Engine engine = new Engine();
        reconnected.connected(backend.component(2), engine, null, false);
        Listening active = take(engine.starts);
        active.listener.onReadyForSpeech(Bundle.EMPTY);
        check(take(listening.events).equals("ready"), "admitted endpoint reconnects with all ten service slots reserved");

        tokens.get(2).die();
        check(take(engine.cancels) == active.listener && take(backend.unbindings) == reconnected
                && tokens.get(2).detached() && backend.process.links() == 9,
                "final owning token death releases the logical service reservation");
        manager.createSession(backend.component(0), replacementToken, false, replacementAdmission);
        IServiceConnection replacementLink = take(backend.bindings);
        replacementLink.connected(backend.component(0), new Engine(), null, false);
        clients.set(2, (IRecognitionService) take(replacementAdmission.events));
        check(!replacementToken.detached() && backend.process.links() == 10,
                "new distinct provider can retry only after the prior endpoint retires");

        for (IRecognitionService client : clients) client.cancel(null, true);
        for (int i = 0; i < 10; i++) take(backend.unbindings);
        check(backend.process.detached() && deniedToken.detached() && failedToken.detached()
                && replacementToken.detached()
                && tokens.stream().allMatch(Lifetime::detached) && backend.unbindings.isEmpty(),
                "all quota scenarios leave no bindings or process/session death links");
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 0) System.loadLibrary("android_runtime");
        else System.load(args[0]);
        manifestEligibility();
        connectionQuota();
        lifecycle();
        supportAndDeath();
        unavailableAndShared();
        java.lang.System.out.println("SpeechRecognitionContractTest all passed: " + checks + " checks");
    }
}
