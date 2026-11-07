/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.microgprobe;

import android.accounts.Account;
import android.accounts.AccountManager;
import android.accounts.AuthenticatorException;
import android.accounts.IAccountManager;
import android.app.Activity;
import android.app.ActivityManager;
import android.app.ActivityThread;
import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.app.PendingIntent;
import android.content.ComponentName;
import android.content.Context;
import android.content.IIntentReceiver;
import android.content.Intent;
import android.content.IntentFilter;
import android.location.ILocationManager;
import android.location.LastLocationRequest;
import android.os.Bundle;
import android.os.ServiceManager;
import android.util.Log;

import java.lang.reflect.InvocationTargetException;
import java.util.Map;
import java.util.concurrent.TimeUnit;

/** Run only in an isolated device with AccountActivity's prepare-probe fixture installed. */
public final class ProbeActivity extends Activity {
    private static final String OWNER = "com.gesher.devsvc";
    private static final String TYPE = "com.gesher.devsvc.account";
    private static final String HIDDEN = "retained-account@example.test";
    private static final String VISIBLE = "visible-ungranted@example.test";
    private interface Operation { void run() throws Exception; }

    private static void pass(String label) { Log.i("microgprobe", "PASS " + label); }
    private static void require(boolean value, String label) {
        if (!value) throw new AssertionError(label);
    }
    private static void denied(String label, Operation operation) throws Exception {
        try { operation.run(); }
        catch (SecurityException expected) { pass(label); return; }
        catch (InvocationTargetException wrapped) {
            if (wrapped.getCause() instanceof SecurityException) { pass(label); return; }
            throw wrapped;
        }
        throw new AssertionError("Unauthorized operation accepted: " + label);
    }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        new Thread(() -> {
            try { probe(); Log.i("microgprobe", "all passed"); }
            catch (Throwable t) {
                Log.e("microgprobe", "FAIL " + t.getClass().getSimpleName() + ": " + t.getMessage());
            } finally { runOnUiThread(this::finish); }
        }, "microg-security-probe").start();
    }

    private void probe() throws Exception {
        require(android.os.Process.myUid() >= android.os.Process.FIRST_APPLICATION_UID
                && getPackageManager().getApplicationInfo(getPackageName(), 0).uid == android.os.Process.myUid(),
                "Probe did not run as its assigned app UID");
        pass("attached unprivileged application identity");
        AccountManager accounts = AccountManager.get(this);
        Account hidden = new Account(HIDDEN, TYPE), visible = new Account(VISIBLE, TYPE);
        if (getIntent().getBooleanExtra("expect-token", false)) {
            String scope = getIntent().getStringExtra("token-type");
            if (scope == null) scope = "probe-ungranted";
            String token = accounts.getAuthToken(visible, scope, null, false, null, null)
                    .getResult(30, TimeUnit.SECONDS).getString(AccountManager.KEY_AUTHTOKEN);
            require(token != null && (token.equals("fake-visible-ungranted-token")
                    || token.startsWith("fixture-token-")), "Explicit credential grant did not authorize token access");
            pass("explicit credential grant authorized an attached client");
            return;
        }

        Account[] found = accounts.getAccountsByType(TYPE);
        boolean sawVisible = false;
        for (Account a : found) {
            require(!HIDDEN.equals(a.name), "hidden account enumeration");
            sawVisible |= VISIBLE.equals(a.name);
        }
        require(sawVisible, "Visible-but-ungranted owner fixture was not prepared");
        pass("hidden account enumeration filtered");
        IAccountManager am = IAccountManager.Stub.asInterface(ServiceManager.getService("account"));
        Map<?, ?> visibility = am.getAccountsAndVisibilityForPackage(getPackageName(), TYPE);
        require(!visibility.containsKey(HIDDEN) && visibility.containsKey(VISIBLE), "own-package hidden account names");
        pass("own-package visibility query filters hidden names");
        denied("owner password read", () -> am.getPassword(hidden));
        denied("owner user data read", () -> am.getUserData(hidden, "server"));
        denied("owner token peek", () -> am.peekAuthToken(hidden, "persisted"));
        denied("owner password mutation", () -> am.setPassword(hidden, "attacker-fixture"));
        denied("owner token mutation", () -> am.setAuthToken(hidden, "persisted", "attacker-fixture"));
        denied("owner account removal", () -> am.removeAccountExplicitly(hidden));
        denied("authenticator account creation", () -> am.addAccountExplicitly(
                new Account("attacker-created@example.test", TYPE), "attacker-fixture", null, getPackageName()));
        denied("self credential grant", () -> am.updateAppPermission(visible, "probe-ungranted",
                android.os.Process.myUid(), true));
        denied("self account visibility mutation", () -> am.setAccountVisibility(visible,
                getPackageName(), AccountManager.VISIBILITY_VISIBLE));
        denied("claimed account package spoof", () -> am.getAccountsAsUser(TYPE, 0, OWNER));
        boolean tokenDenied = false;
        try {
            accounts.getAuthToken(visible, "probe-ungranted", null, false, null, null)
                    .getResult(30, TimeUnit.SECONDS);
        } catch (AuthenticatorException | android.accounts.OperationCanceledException | SecurityException expected) {
            tokenDenied = true;
        }
        require(tokenDenied, "Visibility granted credentials without a token grant");
        pass("visible account without token grant denied");

        IActivityManager activity = IActivityManager.Stub.asInterface(ServiceManager.getService("activity"));
        IApplicationThread thread = (IApplicationThread) ActivityThread.class.getMethod("getApplicationThread")
                .invoke(ActivityThread.currentActivityThread());
        ProviderProbe.run(this, activity, thread);
        IIntentReceiver receiver = new IIntentReceiver.Stub() {
            @Override public void performReceive(Intent i, int code, String data, Bundle extras,
                    boolean ordered, boolean sticky, int user) { }
        };
        denied("broadcast receiver package spoof", () -> activity.registerReceiverWithFeature(thread,
                "com.google.android.gms", null, null, receiver, new IntentFilter("com.gesher.probe.SPOOF"),
                null, 0, Context.RECEIVER_EXPORTED));
        denied("protected cross-app C2DM receive spoof", () -> activity.broadcastIntentWithFeature(thread,
                null, new Intent("com.google.android.c2dm.intent.RECEIVE").setPackage("com.google.android.gms"),
                null, null, 0, null, null, null, null, null, -1, null, false, false, 0));
        for (int flags : new int[] {PendingIntent.FLAG_NO_CREATE, 0, PendingIntent.FLAG_UPDATE_CURRENT}) {
            denied("PendingIntent creator spoof flags=" + flags, () -> activity.getIntentSenderWithFeature(
                    ActivityManager.INTENT_SENDER_BROADCAST, OWNER, null, null, null, 93217,
                    new Intent[] {new Intent("com.gesher.devsvc.ACCOUNT_PROBE_PENDING").setPackage(OWNER)},
                    null, flags | PendingIntent.FLAG_IMMUTABLE, null, 0));
        }
        denied("private owner activity", () -> startActivity(new Intent().setComponent(
                new ComponentName(OWNER, OWNER + ".AccountTokenActivity"))));
        denied("private owner service", () -> startService(new Intent().setComponent(
                new ComponentName(OWNER, OWNER + ".PrivateAccountService"))));
        ComponentName privateService = new ComponentName(OWNER, OWNER + ".PrivateAccountService");
        require(!getPackageManager().getServiceInfo(privateService, 0).exported,
                "Owner stop-service fixture must be installed and private");
        denied("private owner service stop", () -> stopService(new Intent().setComponent(privateService)));
        denied("cross-package force stop", () -> activity.forceStopPackage(OWNER, 0));

        ILocationManager location = ILocationManager.Stub.asInterface(ServiceManager.getService(Context.LOCATION_SERVICE));
        require(location != null, "Location service missing");
        for (String claimed : new String[] {getPackageName(), "android", "com.google.android.gms"}) {
            denied("location request without permission pkg=" + claimed,
                    () -> location.getLastLocation("network", new LastLocationRequest.Builder().build(), claimed, null));
        }

        Class<?> control = Class.forName("com.gesher.server.GesherService$Client");
        Object host = control.getMethod("find").invoke(null);
        require(host != null, "Host control service missing");
        denied("host status", () -> control.getMethod("status").invoke(host));
        denied("host install", () -> control.getMethod("install", String.class).invoke(host, "/not-a-real-probe.apk"));
        denied("host uninstall", () -> control.getMethod("uninstall", String.class).invoke(host, "test.missing.package"));
        denied("host proxy mutation", () -> control.getMethod("setProxy", String.class, String.class).invoke(host, OWNER, "direct"));
        denied("host window mutation", () -> control.getMethod("setWindowMode", String.class, String.class).invoke(host, OWNER, "headless"));
        NotificationProbe.run(this);
        JobProbe.run(this);
    }
}
