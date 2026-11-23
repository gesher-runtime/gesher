/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.IBinderSession;
import android.app.IServiceConnection;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;
import android.os.IBinder;
import android.os.RemoteException;
import android.service.textservice.SpellCheckerService;

import com.android.internal.textservice.ISpellCheckerService;
import com.android.internal.textservice.ISpellCheckerServiceCallback;
import com.android.internal.textservice.ISpellCheckerSession;
import com.android.internal.textservice.ISpellCheckerSessionListener;
import com.android.internal.textservice.ITextServicesSessionListener;
import com.gesher.server.SpellCheckerCatalog.Entry;

import java.lang.ref.WeakReference;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ScheduledThreadPoolExecutor;

/** One owned provider binding with independent client sessions, as in Android textservices. */
final class SpellCheckerSessions {
    private final ActivityManagerService mActivity;
    private final SpellCheckerCatalog mCatalog;
    // Connection callbacks can be synchronous. Serialize transitions off Binder threads,
    // and don't perform application calls while holding the PackageManager or lifecycle lock.
    private final ScheduledThreadPoolExecutor mWorker = new ScheduledThreadPoolExecutor(1, task -> {
        Thread thread = new Thread(task, "gesher-spell-checker");
        thread.setDaemon(true);
        return thread;
    });
    private final Map<IBinder, Request> mRequests = new HashMap<>(); // worker only
    private final Map<ComponentName, Binding> mBindings = new HashMap<>(); // worker only

    private final class Binding {
        final Entry entry;
        final ComponentName component;
        final Set<Request> requests = new LinkedHashSet<>();
        final Set<Request> pending = new LinkedHashSet<>();
        final IBinder.DeathRecipient death = () -> mWorker.execute(() -> retire(this));
        final IServiceConnection connection = new IServiceConnection.Stub() {
            @Override public void connected(ComponentName component, IBinder service,
                    IBinderSession session, boolean dead) {
                mWorker.execute(() -> providerConnected(Binding.this, service, dead));
            }
        };
        boolean bound;
        boolean released;
        IBinder provider;
        ISpellCheckerService checker;

        Binding(Entry entry) {
            this.entry = entry;
            component = entry.info.getComponent();
        }
    }

    private final class Request {
        final Entry entry;
        final int pid;
        final String locale;
        final ITextServicesSessionListener ready;
        final ISpellCheckerSessionListener listener;
        final Bundle extras;
        final int attributes;
        final IBinder client;
        final IBinder.DeathRecipient death = () -> mWorker.execute(() -> retire(this, true));
        Binding binding;
        ISpellCheckerSession session;

        Request(Entry entry, int pid, String locale, ITextServicesSessionListener ready,
                ISpellCheckerSessionListener listener, Bundle extras, int attributes) {
            this.entry = entry;
            this.pid = pid;
            this.locale = locale;
            this.ready = ready;
            this.listener = listener;
            this.extras = extras == null ? null : new Bundle(extras);
            this.attributes = attributes;
            this.client = listener.asBinder();
        }
    }

    /** A provider retaining a completed/canceled callback must not retain its client or group. */
    private static final class SessionCallback extends ISpellCheckerServiceCallback.Stub {
        private WeakReference<SpellCheckerSessions> owner;
        private WeakReference<Request> request;

        SessionCallback(SpellCheckerSessions owner, Request request) {
            this.owner = new WeakReference<>(owner);
            this.request = new WeakReference<>(request);
        }

        @Override public void onSessionCreated(ISpellCheckerSession session) {
            SpellCheckerSessions target;
            Request pending;
            synchronized (this) {
                // Android's callback consumes its request once. In particular, do not
                // close a duplicate Binder that may be the already-delivered session.
                if (request == null) return;
                target = owner.get();
                pending = request.get();
                owner = null;
                request = null;
            }
            if (target == null || pending == null) close(session);
            else target.mWorker.execute(() -> target.complete(pending, session));
        }
    }

    SpellCheckerSessions(ActivityManagerService activity, SpellCheckerCatalog catalog) {
        mActivity = activity;
        mCatalog = catalog;
        mWorker.setRemoveOnCancelPolicy(true);
    }

    void request(Entry entry, int pid, String locale, ITextServicesSessionListener ready,
            ISpellCheckerSessionListener listener, Bundle extras, int attributes) {
        Request request = new Request(entry, pid, locale, ready, listener, extras, attributes);
        mWorker.execute(() -> bind(request));
    }

    void packageRemoving(String packageName) {
        // Capture the actual generation before queueing. A late removal task must
        // not close a newly installed provider with the same package/component.
        ApkPackage retiring = mCatalog.owner(packageName);
        if (retiring == null) return;
        mWorker.execute(() -> {
            for (Binding binding : new ArrayList<>(mBindings.values())) {
                if (binding.entry.owner == retiring) retire(binding);
            }
        });
    }

    void finish(int pid, IBinder listener) {
        mWorker.execute(() -> {
            Request request = mRequests.get(listener);
            if (request != null && request.pid == pid) retire(request, false);
        });
    }

    private void bind(Request request) {
        if (mRequests.containsKey(request.client) || !mCatalog.current(request.entry)) return;
        try {
            request.client.linkToDeath(request.death, 0);
        } catch (RemoteException e) {
            return;
        }
        mRequests.put(request.client, request);
        ComponentName component = request.entry.info.getComponent();
        Binding binding = mBindings.get(component);
        if (binding != null && binding.entry.owner != request.entry.owner) {
            retire(binding);
            binding = null;
        }
        boolean first = binding == null;
        if (first) {
            binding = new Binding(request.entry);
            mBindings.put(component, binding);
        }
        request.binding = binding;
        binding.requests.add(request);
        binding.pending.add(request);
        if (!first) {
            if (binding.checker != null) dispatch(binding, request);
            return;
        }
        Intent intent = new Intent(SpellCheckerService.SERVICE_INTERFACE).setComponent(component);
        Entry selected = binding.entry;
        try {
            binding.bound = mActivity.bindSpellChecker(intent, binding.connection,
                    () -> mCatalog.ownerCurrent(selected)) > 0;
        } catch (RuntimeException failure) {
            // Bind may have published a connection before process-launch failure.
            binding.bound = true;
            retire(binding);
            return;
        }
        if (!binding.bound) retire(binding);
    }

    private void providerConnected(Binding binding, IBinder provider, boolean dead) {
        if (mBindings.get(binding.component) != binding || binding.released) return;
        if (dead || provider == null || !mCatalog.current(binding.entry)) {
            retire(binding);
            return;
        }
        if (binding.provider != null) return;
        try {
            provider.linkToDeath(binding.death, 0);
            binding.provider = provider;
            binding.checker = ISpellCheckerService.Stub.asInterface(provider);
        } catch (RemoteException | RuntimeException failure) {
            retire(binding);
            return;
        }
        while (!binding.pending.isEmpty() && !binding.released) {
            dispatch(binding, binding.pending.iterator().next());
        }
    }

    private void dispatch(Binding binding, Request request) {
        if (!binding.pending.remove(request)) return;
        if (!mCatalog.current(binding.entry)) {
            retire(binding);
            return;
        }
        try {
            binding.checker.getISpellCheckerSession(request.locale, request.listener,
                    request.extras, request.attributes, new SessionCallback(this, request));
        } catch (RemoteException failure) {
            // A dead provider retires its shared binding, not another provider's sessions.
            retire(binding);
        } catch (RuntimeException failure) {
            retire(request, true);
        }
    }

    private void complete(Request request, ISpellCheckerSession session) {
        if (request.session != null && session != null
                && request.session.asBinder().equals(session.asBinder())) return;
        if (mRequests.get(request.client) != request || request.session != null) {
            close(session);
            return;
        }
        if (session == null || !mCatalog.current(request.entry)) {
            close(session);
            retire(request, false);
            return;
        }
        request.session = session;
        try {
            request.ready.onServiceConnected(session);
        } catch (RemoteException | RuntimeException failure) {
            retire(request, true);
        }
    }

    private void retire(Request request, boolean closeDeliveredSession) {
        if (!mRequests.remove(request.client, request)) return;
        unlink(request.client, request.death);
        Binding binding = request.binding;
        binding.requests.remove(request);
        binding.pending.remove(request);
        // Explicit close already sends onClose via Android's SpellCheckerSession before
        // calling finishSpellCheckerService. Only the death/error paths own that message.
        if (closeDeliveredSession) close(request.session);
        request.session = null;
        if (binding.requests.isEmpty()) release(binding);
    }

    private void retire(Binding binding) {
        if (binding.released) return;
        mBindings.remove(binding.component, binding);
        for (Request request : new ArrayList<>(binding.requests)) retire(request, true);
        release(binding);
    }

    private void release(Binding binding) {
        if (binding.released) return;
        binding.released = true;
        mBindings.remove(binding.component, binding);
        if (binding.provider != null) unlink(binding.provider, binding.death);
        binding.provider = null;
        binding.checker = null;
        if (binding.bound) {
            binding.bound = false;
            try { mActivity.unbindService(binding.connection); }
            catch (RuntimeException failure) {
                SystemServer.log("textservices: could not release spell-checker binding: "
                        + failure.getClass().getSimpleName());
            }
        }
    }

    private static void unlink(IBinder binder, IBinder.DeathRecipient death) {
        try { binder.unlinkToDeath(death, 0); }
        catch (java.util.NoSuchElementException alreadyUnlinked) { }
    }

    private static void close(ISpellCheckerSession session) {
        if (session == null) return;
        try { session.onClose(); }
        catch (RemoteException | RuntimeException ignored) { }
    }
}
