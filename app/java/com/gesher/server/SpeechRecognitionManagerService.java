/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.Manifest;
import android.app.IBinderSession;
import android.app.IServiceConnection;
import android.content.AttributionSource;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.content.res.Resources;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Process;
import android.os.RemoteException;
import android.speech.IModelDownloadListener;
import android.speech.IRecognitionListener;
import android.speech.IRecognitionService;
import android.speech.IRecognitionServiceManager;
import android.speech.IRecognitionServiceManagerCallback;
import android.speech.IRecognitionSupportCallback;
import android.speech.RecognitionService;
import android.speech.RecognitionSupport;
import android.speech.SpeechRecognizer;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.NoSuchElementException;
import java.util.Set;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/** Binds the installed RecognitionService selected by the framework or the caller. */
public final class SpeechRecognitionManagerService extends IRecognitionServiceManager.Stub {
    private static final int BIND_TIMEOUT_MS = 20_000;
    private static final int TEMPORARY_PROVIDER_MS = 60_000;
    private static final int MAX_CONNECTIONS_PER_UID = 10;

    public record Owner(int pid, int uid, String packageName, Object code, IBinder process) {}
    public record Provider(ComponentName component, Object code) {}

    /** Device and Binder identity boundary. All service state lives on the worker. */
    public interface Backend {
        Owner caller();
        boolean isCurrent(Owner owner);
        boolean canManage(Owner owner);
        AttributionSource authenticate(Owner owner, AttributionSource source);
        ComponentName onDeviceComponent();
        Provider resolve(ComponentName component);
        boolean isCurrent(Provider provider);
        boolean bind(Provider provider, IServiceConnection connection);
        void unbind(IServiceConnection connection);
    }

    private final Backend mBackend;
    private final ScheduledThreadPoolExecutor mWorker;
    private final int mBindTimeoutMs;
    private final Map<IBinder, Session> mSessions = new HashMap<>();
    private record ConnectionKey(int uid, ComponentName component) {}
    private final Map<ConnectionKey, Binding> mBindings = new HashMap<>();
    private ComponentName mTemporaryComponent;
    private ScheduledFuture<?> mTemporaryExpiry;

    SpeechRecognitionManagerService(ActivityManagerService am, ActivityTaskManagerService atm,
            PackageManagerService pm) {
        this(new RuntimeBackend(am, atm, pm), BIND_TIMEOUT_MS);
        pm.onPackageRemoval(name -> {
            ApkPackage retiring = pm.get(name);
            mWorker.execute(() -> {
                for (Session session : List.copyOf(mSessions.values())) {
                    if (retiring != null && session.owner.code() == retiring) {
                        session.close();
                    } else if (session.binding != null && session.binding.selected.code() == retiring) {
                        session.disconnect(SpeechRecognizer.ERROR_SERVER_DISCONNECTED);
                    }
                }
            });
        });
    }

    // The contract fixture is loaded from a different DEX class loader on ART.
    public SpeechRecognitionManagerService(Backend backend, int bindTimeoutMs) {
        mBackend = backend;
        mBindTimeoutMs = bindTimeoutMs;
        mWorker = new ScheduledThreadPoolExecutor(1, task -> {
            Thread thread = new Thread(task, "gesher-speech-recognition");
            thread.setDaemon(true);
            return thread;
        });
        mWorker.setRemoveOnCancelPolicy(true);
    }

    @Override public void createSession(ComponentName component, IBinder clientToken,
            boolean onDevice, IRecognitionServiceManagerCallback callback) {
        if (callback == null) return;
        Owner owner = mBackend.caller();
        mWorker.execute(() -> createSession(owner, component, clientToken, onDevice, callback));
    }

    private void createSession(Owner owner, ComponentName component, IBinder token,
            boolean onDevice, IRecognitionServiceManagerCallback callback) {
        if (!mBackend.isCurrent(owner) || token == null
                || (!onDevice && Process.isIsolated(owner.uid()))) {
            call(() -> callback.onError(SpeechRecognizer.ERROR_CLIENT));
            return;
        }
        ComponentName selected = onDevice ? (mTemporaryComponent == null
                ? mBackend.onDeviceComponent() : mTemporaryComponent) : component;
        if (selected == null) {
            call(() -> callback.onError(SpeechRecognizer.ERROR_CLIENT));
            return;
        }
        Session existing = mSessions.get(token);
        if (existing != null) {
            if (!existing.owner.equals(owner) || !existing.component.equals(selected)) {
                call(() -> callback.onError(SpeechRecognizer.ERROR_CLIENT));
            } else if (existing.delivered) {
                call(() -> callback.onSuccess(existing));
            } else {
                existing.callbacks.add(callback);
            }
            return;
        }
        Provider provider = mBackend.resolve(selected);
        // Android checks the UID's remote-connection limit before reusing a
        // provider for a new session. Session tokens alone have no such cap.
        if (provider == null || atConnectionLimit(owner.uid())) {
            call(() -> callback.onError(SpeechRecognizer.ERROR_TOO_MANY_REQUESTS));
            return;
        }
        Session session = new Session(owner, selected, token);
        try {
            token.linkToDeath(session.clientDeath, 0);
            if (session.processDeathLink != null) {
                session.processDeathLink.linkToDeath(session.clientDeath, 0);
            }
        } catch (RemoteException failure) {
            unlink(token, session.clientDeath);
            return;
        }
        mSessions.put(token, session);
        session.callbacks.add(callback);
        session.connect(provider);
    }

    @Override public void setTemporaryComponent(ComponentName component) {
        if (!mBackend.canManage(mBackend.caller())) {
            throw new SecurityException("MANAGE_SPEECH_RECOGNITION required");
        }
        mWorker.execute(() -> {
            if (component != null && mBackend.resolve(component) == null) return;
            if (mTemporaryExpiry != null) mTemporaryExpiry.cancel(false);
            mTemporaryComponent = component;
            mTemporaryExpiry = component == null ? null : mWorker.schedule(() -> {
                mTemporaryComponent = null;
                mTemporaryExpiry = null;
            }, TEMPORARY_PROVIDER_MS, TimeUnit.MILLISECONDS);
        });
    }

    @FunctionalInterface private interface RemoteCall { void run() throws RemoteException; }

    private static boolean call(RemoteCall action) {
        try {
            action.run();
            return true;
        } catch (RemoteException | RuntimeException failure) {
            return false;
        }
    }

    private static void unlink(IBinder binder, IBinder.DeathRecipient death) {
        try { binder.unlinkToDeath(death, 0); }
        catch (NoSuchElementException alreadyDead) { /* Death already consumed the link. */ }
    }

    private final class Session extends IRecognitionService.Stub {
        final Owner owner;
        final ComponentName component;
        final IBinder token;
        final IBinder processDeathLink;
        final IBinder.DeathRecipient clientDeath;
        final List<IRecognitionServiceManagerCallback> callbacks = new ArrayList<>();
        final Set<Request> requests = new LinkedHashSet<>();
        Binding binding;
        Recognition recognition;
        boolean delivered;
        boolean closed;

        Session(Owner owner, ComponentName component, IBinder token) {
            this.owner = owner;
            this.component = component;
            this.token = token;
            processDeathLink = token.equals(owner.process()) ? null : owner.process();
            clientDeath = () -> mWorker.execute(this::close);
        }

        private void requireOwner() {
            if (!owner.equals(mBackend.caller())) {
                throw new SecurityException("Recognition session belongs to another process");
            }
        }

        boolean current() {
            if (closed) return false;
            if (!mBackend.isCurrent(owner)) {
                close();
                return false;
            }
            return true;
        }

        void connect() {
            if (binding == null && current()) connect(mBackend.resolve(component));
        }

        void connect(Provider selected) {
            if (selected == null) {
                disconnect(delivered ? SpeechRecognizer.ERROR_SERVER_DISCONNECTED
                        : SpeechRecognizer.ERROR_CLIENT);
                return;
            }
            attach(this, selected);
        }

        void deliver(Binding connected) {
            if (binding != connected || !current()) return;
            if (!delivered) {
                for (IRecognitionServiceManagerCallback callback : callbacks) {
                    delivered |= call(() -> callback.onSuccess(Session.this));
                }
                callbacks.clear();
                if (!delivered) {
                    close();
                    return;
                }
            }
            dispatch();
        }

        void dispatch() {
            Binding connected = binding;
            if (connected == null || connected.service == null) return;
            if (!mBackend.isCurrent(connected.selected)) {
                connected.failAll(SpeechRecognizer.ERROR_SERVER_DISCONNECTED);
                return;
            }
            IRecognitionService provider = connected.service;
            if (recognition != null && !recognition.dispatch(provider)) {
                disconnect(SpeechRecognizer.ERROR_SERVER_DISCONNECTED);
                return;
            }
            for (Request request : List.copyOf(requests)) {
                if (!request.dispatch(provider)) {
                    disconnect(SpeechRecognizer.ERROR_SERVER_DISCONNECTED);
                    return;
                }
            }
        }

        void disconnect(int error) {
            Recognition previousRecognition = recognition;
            cancelRecognition(true);
            Binding previous = binding;
            binding = null;
            if (previous != null) previous.detach(this);
            if (!delivered) {
                List<IRecognitionServiceManagerCallback> failed = List.copyOf(callbacks);
                close();
                for (IRecognitionServiceManagerCallback callback : failed) {
                    call(() -> callback.onError(error));
                }
                return;
            }
            if (previousRecognition != null) {
                call(() -> previousRecognition.listener.onError(error));
            }
            for (Request request : requests) {
                request.retire();
                call(() -> request.error(error));
            }
            requests.clear();
        }

        void close() {
            if (closed) return;
            closed = true;
            mSessions.remove(token, this);
            unlink(token, clientDeath);
            if (processDeathLink != null) unlink(processDeathLink, clientDeath);
            callbacks.clear();
            for (Request request : requests) request.retire();
            requests.clear();
            cancelRecognition(true);
            if (binding != null) {
                Binding previous = binding;
                binding = null;
                previous.detach(this);
            }
        }

        private void cancelRecognition(boolean shutdown) {
            Recognition previous = recognition;
            recognition = null;
            if (previous != null && previous.started && binding != null && binding.service != null) {
                call(() -> binding.service.cancel(previous, shutdown));
            }
            if (previous != null) previous.retire();
        }

        @Override public void startListening(Intent intent, IRecognitionListener listener,
                AttributionSource source) {
            requireOwner();
            AttributionSource attribution = mBackend.authenticate(owner, source);
            if (listener == null) return;
            mWorker.execute(() -> {
                if (!current()) return;
                if (recognition != null || intent == null) {
                    call(() -> listener.onError(SpeechRecognizer.ERROR_CLIENT));
                    return;
                }
                recognition = new Recognition(listener, intent, attribution);
                connect();
                dispatch();
            });
        }

        @Override public void stopListening(IRecognitionListener listener) {
            requireOwner();
            if (listener == null) return;
            mWorker.execute(() -> {
                if (!current()) return;
                Recognition active = recognition;
                if (active == null || active.stopRequested || !active.matches(listener)) {
                    call(() -> listener.onError(SpeechRecognizer.ERROR_CLIENT));
                    return;
                }
                active.stopRequested = true;
                dispatch();
            });
        }

        @Override public void cancel(IRecognitionListener listener, boolean shutdown) {
            requireOwner();
            mWorker.execute(() -> {
                if (!current()) return;
                if (shutdown) close();
                else if (recognition != null && recognition.matches(listener)) cancelRecognition(false);
            });
        }

        @Override public void checkRecognitionSupport(Intent intent, AttributionSource source,
                IRecognitionSupportCallback callback) {
            requireOwner();
            AttributionSource attribution = mBackend.authenticate(owner, source);
            if (callback == null) return;
            enqueue(new SupportRequest(intent, attribution, callback));
        }

        @Override public void triggerModelDownload(Intent intent, AttributionSource source,
                IModelDownloadListener listener) {
            requireOwner();
            enqueue(new ModelRequest(intent, mBackend.authenticate(owner, source), listener));
        }

        private void enqueue(Request request) {
            mWorker.execute(() -> {
                if (!current()) return;
                if (request.intent == null) {
                    call(() -> request.error(SpeechRecognizer.ERROR_CLIENT));
                    return;
                }
                requests.add(request);
                connect();
                dispatch();
            });
        }

        /** A new Binder per listening attempt, to reject late callbacks from older attempts. */
        final class Recognition extends IRecognitionListener.Stub {
            final IRecognitionListener listener;
            Intent intent;
            AttributionSource attribution;
            boolean started;
            boolean stopRequested;
            boolean stopSent;

            Recognition(IRecognitionListener listener, Intent intent, AttributionSource attribution) {
                this.listener = listener;
                this.intent = intent;
                this.attribution = attribution;
            }

            boolean matches(IRecognitionListener other) {
                return other != null && listener.asBinder().equals(other.asBinder());
            }

            boolean dispatch(IRecognitionService service) {
                if (!started) {
                    started = true;
                    boolean sent = call(() -> service.startListening(intent, this, attribution));
                    intent = null;
                    if (!sent) return false;
                }
                if (!stopRequested || stopSent) return true;
                stopSent = true;
                return call(() -> service.stopListening(this));
            }

            private void emit(boolean terminal, RemoteCall action) {
                mWorker.execute(() -> {
                    if (!current() || recognition != this) return;
                    if (!mBackend.isCurrent(binding.selected)) {
                        disconnect(SpeechRecognizer.ERROR_SERVER_DISCONNECTED);
                        return;
                    }
                    if (terminal) {
                        recognition = null;
                        retire();
                    }
                    if (!call(action) && !terminal) cancelRecognition(false);
                });
            }

            void retire() {
                intent = null;
                attribution = null;
            }

            @Override public void onReadyForSpeech(Bundle value) {
                emit(false, () -> listener.onReadyForSpeech(value));
            }
            @Override public void onBeginningOfSpeech() { emit(false, listener::onBeginningOfSpeech); }
            @Override public void onRmsChanged(float value) { emit(false, () -> listener.onRmsChanged(value)); }
            @Override public void onBufferReceived(byte[] value) {
                emit(false, () -> listener.onBufferReceived(value));
            }
            @Override public void onEndOfSpeech() { emit(false, listener::onEndOfSpeech); }
            @Override public void onError(int value) { emit(true, () -> listener.onError(value)); }
            @Override public void onResults(Bundle value) { emit(true, () -> listener.onResults(value)); }
            @Override public void onPartialResults(Bundle value) {
                emit(false, () -> listener.onPartialResults(value));
            }
            @Override public void onSegmentResults(Bundle value) {
                emit(false, () -> listener.onSegmentResults(value));
            }
            @Override public void onEndOfSegmentedSession() {
                emit(true, listener::onEndOfSegmentedSession);
            }
            @Override public void onLanguageDetection(Bundle value) {
                emit(false, () -> listener.onLanguageDetection(value));
            }
            @Override public void onEvent(int event, Bundle value) {
                emit(false, () -> listener.onEvent(event, value));
            }
        }

        abstract class Request {
            Intent intent;
            AttributionSource attribution;
            boolean started;

            Request(Intent intent, AttributionSource attribution) {
                this.intent = intent;
                this.attribution = attribution;
            }

            final boolean dispatch(IRecognitionService provider) {
                if (started) return true;
                started = true;
                boolean sent = call(() -> send(provider));
                intent = null;
                return sent;
            }

            final void emit(boolean terminal, RemoteCall event) {
                mWorker.execute(() -> {
                    if (!current() || !requests.contains(this)) return;
                    if (!mBackend.isCurrent(binding.selected)) {
                        disconnect(SpeechRecognizer.ERROR_SERVER_DISCONNECTED);
                        return;
                    }
                    if (terminal) complete();
                    if (!call(event) && !terminal) complete();
                });
            }

            final void complete() {
                requests.remove(this);
                retire();
            }

            final void retire() {
                intent = null;
                attribution = null;
            }

            abstract void send(IRecognitionService provider) throws RemoteException;
            abstract void error(int code) throws RemoteException;
        }

        final class SupportRequest extends Request {
            final IRecognitionSupportCallback target;
            final IRecognitionSupportCallback proxy = new IRecognitionSupportCallback.Stub() {
                @Override public void onSupportResult(RecognitionSupport support) {
                    emit(true, () -> target.onSupportResult(support));
                }
                @Override public void onError(int code) { emit(true, () -> target.onError(code)); }
            };

            SupportRequest(Intent intent, AttributionSource attribution,
                    IRecognitionSupportCallback target) {
                super(intent, attribution);
                this.target = target;
            }

            @Override void send(IRecognitionService provider) throws RemoteException {
                provider.checkRecognitionSupport(intent, attribution, proxy);
            }
            @Override void error(int code) throws RemoteException { target.onError(code); }
        }

        final class ModelRequest extends Request {
            final IModelDownloadListener target;
            final IModelDownloadListener proxy = new IModelDownloadListener.Stub() {
                @Override public void onProgress(int percent) {
                    emit(false, () -> target.onProgress(percent));
                }
                @Override public void onSuccess() { emit(true, target::onSuccess); }
                @Override public void onScheduled() { emit(true, target::onScheduled); }
                @Override public void onError(int code) { emit(true, () -> target.onError(code)); }
            };

            ModelRequest(Intent intent, AttributionSource attribution, IModelDownloadListener target) {
                super(intent, attribution);
                this.target = target;
            }

            @Override void send(IRecognitionService provider) throws RemoteException {
                provider.triggerModelDownload(intent, attribution, target == null ? null : proxy);
                if (target == null) complete();
            }
            @Override void error(int code) throws RemoteException {
                if (target != null) target.onError(code);
            }
        }
    }

    private boolean atConnectionLimit(int uid) {
        // A delivered endpoint retains its logical connection reservation
        // across provider loss. Only final session retirement releases it.
        Set<ComponentName> components = new LinkedHashSet<>();
        for (Session session : mSessions.values()) {
            if (session.owner.uid() == uid && components.add(session.component)
                    && components.size() >= MAX_CONNECTIONS_PER_UID) return true;
        }
        return false;
    }

    private void attach(Session session, Provider selected) {
        ConnectionKey key = new ConnectionKey(session.owner.uid(), selected.component());
        Binding binding = mBindings.get(key);
        if (binding != null && !mBackend.isCurrent(binding.selected)) {
            binding.failAll(SpeechRecognizer.ERROR_SERVER_DISCONNECTED);
            binding = null;
        }
        if (binding == null) {
            binding = new Binding(key, selected);
            mBindings.put(key, binding);
        }
        binding.attach(session);
    }

    /** One remote provider connection per caller UID and provider component. */
    private final class Binding {
        final ConnectionKey key;
        final Provider selected;
        final Set<Session> sessions = new LinkedHashSet<>();
        final IServiceConnection connection = new IServiceConnection.Stub() {
            @Override public void connected(ComponentName name, IBinder binder,
                    IBinderSession binderSession, boolean dead) {
                mWorker.execute(() -> publish(name, dead ? null : binder));
            }
        };
        final IBinder.DeathRecipient death = () -> mWorker.execute(() ->
                failAll(SpeechRecognizer.ERROR_SERVER_DISCONNECTED));
        ScheduledFuture<?> timeout;
        IRecognitionService service;
        boolean closed;

        Binding(ConnectionKey key, Provider selected) {
            this.key = key;
            this.selected = selected;
        }

        void attach(Session session) {
            if (closed) return;
            sessions.add(session);
            session.binding = this;
            if (service != null) {
                session.deliver(this);
            } else if (sessions.size() == 1) {
                timeout = mWorker.schedule(() -> failAll(SpeechRecognizer.ERROR_CLIENT),
                        mBindTimeoutMs, TimeUnit.MILLISECONDS);
                if (!mBackend.bind(selected, connection)) {
                    failAll(SpeechRecognizer.ERROR_CLIENT);
                }
            }
        }

        private void publish(ComponentName name, IBinder binder) {
            if (closed || mBindings.get(key) != this) return;
            if (binder == null || !selected.component().equals(name) || !mBackend.isCurrent(selected)) {
                failAll(SpeechRecognizer.ERROR_SERVER_DISCONNECTED);
                return;
            }
            if (service != null) return;
            try {
                binder.linkToDeath(death, 0);
            } catch (RemoteException failure) {
                failAll(SpeechRecognizer.ERROR_SERVER_DISCONNECTED);
                return;
            }
            if (timeout != null) timeout.cancel(false);
            service = IRecognitionService.Stub.asInterface(binder);
            for (Session session : List.copyOf(sessions)) session.deliver(this);
        }

        void detach(Session session) {
            if (closed || !sessions.remove(session)) return;
            if (sessions.isEmpty()) close();
        }

        void failAll(int error) {
            if (closed) return;
            List<Session> affected = List.copyOf(sessions);
            close();
            for (Session session : affected) {
                if (session.binding == this) session.disconnect(error);
            }
        }

        private void close() {
            if (closed) return;
            closed = true;
            mBindings.remove(key, this);
            if (timeout != null) timeout.cancel(false);
            if (service != null) unlink(service.asBinder(), death);
            mBackend.unbind(connection);
            sessions.clear();
            service = null;
        }
    }

    private static final class RuntimeBackend implements Backend {
        private final ActivityManagerService am;
        private final ActivityTaskManagerService atm;
        private final PackageManagerService pm;
        private final CallerIdentity callers;

        RuntimeBackend(ActivityManagerService am, ActivityTaskManagerService atm, PackageManagerService pm) {
            this.am = am;
            this.atm = atm;
            this.pm = pm;
            callers = am.callers();
        }

        @Override public Owner caller() {
            String name = callers.callerPackage();
            int pid = callers.callerPid();
            ActivityTaskManagerService.ProcessRecord process = "android".equals(name)
                    ? null : atm.callerProcess(pid);
            return new Owner(pid, callers.callerUid(), name, pm.get(name),
                    process == null ? null : process.thread.asBinder());
        }

        @Override public boolean isCurrent(Owner owner) {
            String name = owner.packageName();
            if ("android".equals(name)) return owner.code() == null;
            if (owner.process() == null || owner.code() == null || owner.code() != pm.get(name)
                    || pm.isRemovalPending(name)) return false;
            ActivityTaskManagerService.ProcessRecord process = atm.callerProcess(owner.pid());
            return process != null && !process.dead && !process.exiting
                    && name.equals(process.packageName) && owner.process().equals(process.thread.asBinder());
        }

        @Override public boolean canManage(Owner owner) {
            return "android".equals(owner.packageName()) || pm.permissions().checkPermission(
                    owner.packageName(), Manifest.permission.MANAGE_SPEECH_RECOGNITION)
                    == PackageManager.PERMISSION_GRANTED;
        }

        @Override public AttributionSource authenticate(Owner owner, AttributionSource source) {
            if (source == null || source.getUid() != owner.uid()
                    || !owner.packageName().equals(source.getPackageName())
                    || (source.getPid() != Process.INVALID_PID && source.getPid() != owner.pid())
                    || source.getDeviceId() != Context.DEVICE_ID_DEFAULT) {
                throw new SecurityException("Invalid speech recognition attribution");
            }
            if (!"android".equals(owner.packageName())) {
                int depth = 0;
                for (AttributionSource next = source.getNext(); next != null; next = next.getNext()) {
                    if (++depth >= 16 || !pm.permissions().isRegisteredAttributionSource(next.asState())) {
                        throw new SecurityException("Untrusted recognition attribution chain");
                    }
                }
                if (!pm.permissions().isRegisteredAttributionSource(source.asState())) {
                    source = source.withToken(new Binder());
                    source = source.withToken(pm.permissions().registerAttributionSource(source.asState()));
                }
            }
            // The provider's Binder parcel is sent by system_server, with the app UID
            // authorized above. A caller PID in that parcel would be the wrong hop.
            return new AttributionSource.Builder(source).setPid(Process.INVALID_PID).build();
        }

        @Override public ComponentName onDeviceComponent() {
            return ComponentName.unflattenFromString(Resources.getSystem().getString(
                    com.android.internal.R.string.config_defaultOnDeviceSpeechRecognitionService));
        }

        @Override public Provider resolve(ComponentName component) {
            return installedProvider(pm, component);
        }

        @Override public boolean isCurrent(Provider provider) {
            ApkPackage pkg = (ApkPackage) provider.code();
            return pkg == pm.get(pkg.packageName) && !pm.isRemovalPending(pkg.packageName)
                    && pkg.componentEnabled(provider.component().getClassName(), ApkPackage.ComponentKind.SERVICE);
        }

        @Override public boolean bind(Provider provider, IServiceConnection connection) {
            try {
                return am.bindSystemService(providerIntent().setComponent(provider.component()), connection,
                        () -> isCurrent(provider)) > 0;
            } catch (RuntimeException failure) {
                return false;
            }
        }

        @Override public void unbind(IServiceConnection connection) {
            am.unbindService(connection);
        }
    }

    private static Provider installedProvider(PackageManagerService pm, ComponentName component) {
        synchronized (pm) {
            ApkPackage pkg = pm.get(component.getPackageName());
            if (pkg == null || pm.isRemovalPending(pkg.packageName)) return null;
            for (ResolveInfo match : pkg.queryIntentServices(providerIntent(), null, 0)) {
                ServiceInfo service = match.serviceInfo;
                if (isProvider(service) && component.equals(service.getComponentName())) {
                    return new Provider(component, pkg);
                }
            }
            return null;
        }
    }

    static String configuredDefault(PackageManagerService pm, String configured) {
        if (pm == null || configured == null) return null;
        ComponentName component = ComponentName.unflattenFromString(configured);
        return component == null || installedProvider(pm, component) == null
                ? null : component.flattenToShortString();
    }

    private static Intent providerIntent() { return new Intent(RecognitionService.SERVICE_INTERFACE); }

    public static boolean isProvider(ServiceInfo service) {
        return service != null && service.exported && service.enabled
                && service.applicationInfo != null && service.applicationInfo.enabled;
    }
}
