/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.accounts.Account;
import android.accounts.AccountManager;
import android.content.ComponentName;
import android.content.Intent;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BooleanSupplier;

/** Server-owned AccountManager chooser. Account labels never leave the trusted host dialog. */
final class AccountChooser {
    static final ComponentName COMPONENT = new ComponentName("android",
            "android.accounts.ChooseTypeAndAccountActivity");
    interface Lifetime {
        boolean active();
        boolean whileActive(Runnable action);
    }
    interface Launcher { boolean launch(Intent intent, Runnable finished); }
    interface Result { void finish(Intent data); }
    interface Dialog { int choose(String requester, String[] labels, boolean add, BooleanSupplier active); }
    private static final Dialog HOST = (requester, labels, add, active) -> {
        AccountSecretStore.ensureLoaded();
        return nativeChoose(requester, labels, add, active);
    };
    private static native int nativeChoose(String packageName, String[] labels,
            boolean canAdd, BooleanSupplier isActive);

    static boolean handles(Intent intent) {
        return intent != null && COMPONENT.equals(intent.getComponent());
    }

    private final AtomicBoolean mFinished = new AtomicBoolean();
    private final AccountManagerService.ChooserSession mSession;
    private final Lifetime mLifetime;
    private final Launcher mLauncher;
    private final Result mResult;
    private final Dialog mDialog;

    AccountChooser(AccountManagerService.ChooserSession session, Lifetime lifetime,
            Launcher launcher, Result result) {
        this(session, lifetime, launcher, result, HOST);
    }
    AccountChooser(AccountManagerService.ChooserSession session, Lifetime lifetime,
            Launcher launcher, Result result, Dialog dialog) {
        mSession = session; mLifetime = lifetime; mLauncher = launcher;
        mResult = result; mDialog = dialog;
    }
    void start() {
        Thread worker = new Thread(this::run, "account-chooser");
        worker.setDaemon(true); worker.start();
    }
    private boolean active() { return !mFinished.get() && mLifetime.active() && mSession.active(); }
    private void finish(Intent data) {
        if (mFinished.compareAndSet(false, true)) mResult.finish(data);
    }
    void run() {
        try {
            while (active()) {
                List<Account> accounts = mSession.accounts();
                List<String> types = mSession.types();
                String[] labels = new String[accounts.size()];
                for (int i = 0; i < labels.length; i++) {
                    Account account = accounts.get(i);
                    labels[i] = account.name + " (" + account.type + ")";
                }
                int selected = mDialog.choose(mSession.packageName(), labels,
                        !types.isEmpty(), this::active);
                if (!active()) break;
                if (selected >= 0 && selected < accounts.size()) {
                    Intent result = mSession.select(accounts.get(selected), mLifetime);
                    finish(result); return;
                }
                if (selected != -2 || types.isEmpty()) break;
                // Adding and choosing an account are separate explicit decisions.
                int type = mDialog.choose(mSession.packageName(), types.toArray(new String[0]),
                        false, this::active);
                if (!active() || type < 0 || type >= types.size()) break;
                if (!mSession.add(types.get(type), mLauncher, this::active)) break;
            }
        } catch (RuntimeException | LinkageError e) {
            // Fail closed without putting account labels, Bundles or native errors in logs.
            SystemServer.log("account chooser unavailable: " + e.getClass().getSimpleName());
        }
        finish(null);
    }
}
