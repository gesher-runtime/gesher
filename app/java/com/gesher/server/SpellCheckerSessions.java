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

import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/** Owns one binding per client session. ActiveServices shares the provider's actual Service. */
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
        final IServiceConnection connection = new IServiceConnection.Stub() {
            @Override public void connected(ComponentName component, IBinder service,
                    IBinderSession session, boolean dead) {
                mWorker.execute(() -> providerConnected(Request.this, service, dead));
            }
        };
        boolean bound;
        IBinder provider;
        ISpellCheckerSession session;
        ScheduledFuture<?> timeout;

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
        mWorker.execute(() -> {
            for (Request request : new ArrayList<>(mRequests.values())) {
                if (packageName.equals(request.entry.owner.packageName)) retire(request, true);
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
        Intent intent = new Intent(SpellCheckerService.SERVICE_INTERFACE).setComponent(component);
        try {
            request.bound = mActivity.bindSpellChecker(intent, request.connection,
                    () -> mCatalog.ownerCurrent(request.entry)) > 0;
        } catch (RuntimeException failure) {
            // Bind may have published a connection before process-launch failure.
            request.bound = true;
            retire(request, false);
            return;
        }
        if (!request.bound) {
            retire(request, false);
            return;
        }
        request.timeout = mWorker.schedule(() -> retire(request, false), 30, TimeUnit.SECONDS);
    }

    private void providerConnected(Request request, IBinder provider, boolean dead) {
        if (mRequests.get(request.client) != request) return;
        if (dead || provider == null || !mCatalog.current(request.entry)) {
            retire(request, true);
            return;
        }
        if (request.provider != null) return;
        try {
            provider.linkToDeath(request.death, 0);
            request.provider = provider;
            ISpellCheckerService.Stub.asInterface(provider).getISpellCheckerSession(request.locale,
                    request.listener, request.extras, request.attributes,
                    new ISpellCheckerServiceCallback.Stub() {
                        @Override public void onSessionCreated(ISpellCheckerSession session) {
                            mWorker.execute(() -> complete(request, session));
                        }
                    });
        } catch (RemoteException | RuntimeException failure) {
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
        request.timeout.cancel(false);
        try {
            request.ready.onServiceConnected(session);
        } catch (RemoteException | RuntimeException failure) {
            retire(request, true);
        }
    }

    private void retire(Request request, boolean closeDeliveredSession) {
        if (!mRequests.remove(request.client, request)) return;
        if (request.timeout != null) request.timeout.cancel(false);
        unlink(request.client, request.death);
        if (request.provider != null) unlink(request.provider, request.death);
        // Explicit close already sends onClose via Android's SpellCheckerSession before
        // calling finishSpellCheckerService. Only the death/error paths own that message.
        if (closeDeliveredSession) close(request.session);
        if (request.bound) {
            try { mActivity.unbindService(request.connection); }
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
