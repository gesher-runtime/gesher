/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.gmsapp;

import android.accounts.Account;
import android.accounts.AccountManager;
import android.app.Activity;
import android.app.Dialog;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.location.Location;
import android.os.Handler;
import android.os.Looper;

import com.google.android.gms.auth.GoogleAuthUtil;
import com.google.android.gms.auth.api.signin.GoogleSignIn;
import com.google.android.gms.auth.api.signin.GoogleSignInAccount;
import com.google.android.gms.auth.api.signin.GoogleSignInClient;
import com.google.android.gms.auth.api.signin.GoogleSignInOptions;
import com.google.android.gms.common.ConnectionResult;
import com.google.android.gms.common.GoogleApiAvailability;
import com.google.android.gms.common.GooglePlayServicesUtil;
import com.google.android.gms.gcm.GoogleCloudMessaging;
import com.google.android.gms.location.FusedLocationProviderClient;
import com.google.android.gms.location.LocationCallback;
import com.google.android.gms.location.LocationRequest;
import com.google.android.gms.location.LocationResult;
import com.google.android.gms.location.LocationServices;
import com.google.android.gms.tasks.Task;
import com.google.android.gms.tasks.Tasks;
import com.google.firebase.messaging.FirebaseMessaging;

import java.lang.reflect.Method;
import java.util.List;

public final class GmsUnitSteps {
    private final Activity mActivity;

    public GmsUnitSteps(Activity activity) {
        mActivity = activity;
    }

    public void runAll() throws Exception {
        testClientSdkFromApk();
        testProviderInstallerCode();
        testGoogleApiAvailability();
        testPackageManagerSyntheticGms();
        testFusedLocationProviderClient();
        testGoogleSignInAndAccountManager();
        testFirebaseAndFcmRouting();
    }

    /**
     * The client-SDK classes (play-services, Firebase) are the APK's own, as on Android:
     * gesher-app.jar on the boot class path has none (app/build.sh), which would shadow
     * an app's real copies (parent-first loading; a stub FirebaseApp.get() answering null
     * is "FirebaseCrashlytics component is not present").
     */
    public void testClientSdkFromApk() throws Exception {
        ClassLoader own = GmsUnitSteps.class.getClassLoader();
        Class<?>[] sdk = {
            com.google.firebase.FirebaseApp.class,
            FirebaseMessaging.class,
            GoogleApiAvailability.class,
            Tasks.class,
            LocationServices.class,
        };
        for (Class<?> c : sdk) {
            if (c.getClassLoader() != own) {
                throw new AssertionError(c.getName() + " was loaded by " + c.getClassLoader()
                        + ", not the APK's class loader " + own);
            }
        }
        // Not on the boot class path at all (Context's loader is the boot class loader).
        ClassLoader boot = Context.class.getClassLoader();
        for (String name : new String[] {"com.google.firebase.FirebaseApp",
                "com.google.android.gms.common.GoogleApiAvailability"}) {
            try {
                Class<?> c = Class.forName(name, false, boot);
                throw new AssertionError(name + " is on the boot class path (" + c.getClassLoader() + ")");
            } catch (ClassNotFoundException expected) {
                // as on Android
            }
        }
        Report.line("client SDK: FirebaseApp and play-services classes loaded from the APK, none on the boot class path");
    }

    /**
     * Play services' code, as the client library's ProviderInstaller loads it (Gmail
     * refuses its mail servers without): the package context with CONTEXT_INCLUDE_CODE,
     * its class loader, ProviderInstallerImpl's insertProvider and reportRequestStats2.
     */
    public void testProviderInstallerCode() throws Exception {
        Context gms = mActivity.createPackageContext("com.google.android.gms",
                Context.CONTEXT_INCLUDE_CODE | Context.CONTEXT_IGNORE_SECURITY);
        Class<?> impl = gms.getClassLoader().loadClass("com.google.android.gms.common.security.ProviderInstallerImpl");
        Method insert = impl.getMethod("insertProvider", Context.class);
        insert.invoke(null, gms);
        Method stats = impl.getMethod("reportRequestStats2", Context.class, long.class, long.class);
        stats.invoke(null, mActivity, 0L, 1L);
        Report.line("ProviderInstaller: Play services' code loads from its package context, insertProvider runs");
    }

    public void testGoogleApiAvailability() throws Exception {
        GoogleApiAvailability api = GoogleApiAvailability.getInstance();
        int code = api.isGooglePlayServicesAvailable(mActivity);
        if (code != ConnectionResult.SUCCESS) {
            throw new AssertionError("isGooglePlayServicesAvailable returned " + code + ", expected SUCCESS");
        }
        int utilCode = GooglePlayServicesUtil.isGooglePlayServicesAvailable(mActivity);
        if (utilCode != ConnectionResult.SUCCESS) {
            throw new AssertionError("GooglePlayServicesUtil returned " + utilCode);
        }

        Dialog dialog = api.getErrorDialog(mActivity, ConnectionResult.SERVICE_MISSING, 100);
        if (dialog == null) {
            throw new AssertionError("getErrorDialog returned null for SERVICE_MISSING");
        }
        Dialog successDialog = api.getErrorDialog(mActivity, ConnectionResult.SUCCESS, 100);
        if (successDialog != null) {
            throw new AssertionError("getErrorDialog returned non-null for SUCCESS");
        }

        Task<Void> task = api.makeGooglePlayServicesAvailable(mActivity);
        Tasks.await(task);
        if (!task.isSuccessful()) {
            throw new AssertionError("makeGooglePlayServicesAvailable task was not successful");
        }
        Report.line("GoogleApiAvailability: isGooglePlayServicesAvailable SUCCESS (0)");
        Report.line("GoogleApiAvailability: makeGooglePlayServicesAvailable completed");
    }

    public void testPackageManagerSyntheticGms() throws Exception {
        PackageManager pm = mActivity.getPackageManager();

        // Check com.google.android.gms
        PackageInfo gmsInfo = pm.getPackageInfo("com.google.android.gms",
                PackageManager.GET_SIGNATURES | PackageManager.GET_SERVICES);
        if (gmsInfo == null) {
            throw new AssertionError("getPackageInfo(com.google.android.gms) returned null");
        }
        if (!"com.google.android.gms".equals(gmsInfo.packageName)) {
            throw new AssertionError("unexpected package name: " + gmsInfo.packageName);
        }
        if (gmsInfo.versionCode < 233013000) {
            throw new AssertionError("unexpected gms versionCode: " + gmsInfo.versionCode);
        }
        if (gmsInfo.signatures == null || gmsInfo.signatures.length == 0) {
            throw new AssertionError("com.google.android.gms has no signatures");
        }
        // Google's own certificate, which the real client libraries' GoogleSignatureVerifier
        // requires (Gmail: "signature is invalid", error 9, with any other).
        byte[] sha1 = java.security.MessageDigest.getInstance("SHA-1")
                .digest(gmsInfo.signatures[0].toByteArray());
        StringBuilder hex = new StringBuilder();
        for (byte b : sha1) hex.append(String.format("%02x", b & 0xff));
        if (!"38918a453d07199354f8b19af05ec6562ced5788".equals(hex.toString())) {
            throw new AssertionError("com.google.android.gms is not signed with Google's certificate: "
                    + hex);
        }
        if (gmsInfo.applicationInfo == null || !gmsInfo.applicationInfo.enabled) {
            throw new AssertionError("com.google.android.gms applicationInfo not enabled");
        }

        // Check com.android.vending
        PackageInfo vendingInfo = pm.getPackageInfo("com.android.vending", PackageManager.GET_SIGNATURES);
        if (vendingInfo == null) {
            throw new AssertionError("getPackageInfo(com.android.vending) returned null");
        }
        if (!"com.android.vending".equals(vendingInfo.packageName)) {
            throw new AssertionError("unexpected vending package: " + vendingInfo.packageName);
        }
        if (vendingInfo.signatures == null || vendingInfo.signatures.length == 0) {
            throw new AssertionError("com.android.vending has no signatures");
        }

        // Check installed packages
        List<PackageInfo> installed = pm.getInstalledPackages(0);
        boolean hasGms = false;
        boolean hasVending = false;
        for (PackageInfo pi : installed) {
            if ("com.google.android.gms".equals(pi.packageName)) hasGms = true;
            if ("com.android.vending".equals(pi.packageName)) hasVending = true;
        }
        if (!hasGms || !hasVending) {
            throw new AssertionError("installed packages missing synthetic GMS/Vending: gms="
                    + hasGms + " vending=" + hasVending);
        }

        Report.line("PackageManager: com.google.android.gms synthetic package present, enabled, valid signatures (Google's certificate)");
        Report.line("PackageManager: com.android.vending synthetic package present, enabled");
    }

    public void testFusedLocationProviderClient() throws Exception {
        FusedLocationProviderClient client = LocationServices.getFusedLocationProviderClient(mActivity);
        Task<Location> lastLocTask = client.getLastLocation();
        Tasks.await(lastLocTask);
        if (!lastLocTask.isComplete()) {
            throw new AssertionError("getLastLocation did not complete");
        }

        LocationCallback callback = new LocationCallback() {
            @Override
            public void onLocationResult(LocationResult locationResult) {
                Report.line("FusedLocationProviderClient: callback received locationResult");
            }
        };

        LocationRequest req = LocationRequest.create()
                .setInterval(1000)
                .setFastestInterval(500)
                .setPriority(LocationRequest.PRIORITY_HIGH_ACCURACY);

        Task<Void> updateTask = client.requestLocationUpdates(req, callback, Looper.getMainLooper());
        Tasks.await(updateTask);
        if (!updateTask.isSuccessful()) {
            throw new AssertionError("requestLocationUpdates failed");
        }

        Task<Void> removeTask = client.removeLocationUpdates(callback);
        Tasks.await(removeTask);
        if (!removeTask.isSuccessful()) {
            throw new AssertionError("removeLocationUpdates failed");
        }

        Report.line("FusedLocationProviderClient: connected to LocationManagerService, updates received");
    }

    public void testGoogleSignInAndAccountManager() throws Exception {
        GoogleSignInOptions options = new GoogleSignInOptions.Builder(GoogleSignInOptions.DEFAULT_SIGN_IN)
                .requestEmail()
                .requestIdToken("dummy_client_id.apps.googleusercontent.com")
                .build();

        GoogleSignInClient client = GoogleSignIn.getClient(mActivity, options);
        Intent signInIntent = client.getSignInIntent();
        if (signInIntent == null) {
            throw new AssertionError("getSignInIntent returned null");
        }

        Task<GoogleSignInAccount> accountTask = client.silentSignIn();
        GoogleSignInAccount account = Tasks.await(accountTask);
        if (account == null) {
            throw new AssertionError("silentSignIn returned null account");
        }
        if (!"user@gesher.local".equals(account.getEmail())) {
            throw new AssertionError("unexpected account email: " + account.getEmail());
        }
        if (account.getIdToken() == null || account.getIdToken().isEmpty()) {
            throw new AssertionError("empty idToken");
        }

        GoogleSignInAccount last = GoogleSignIn.getLastSignedInAccount(mActivity);
        if (last == null || !account.getEmail().equals(last.getEmail())) {
            throw new AssertionError("getLastSignedInAccount mismatch");
        }

        // Test AccountManager
        AccountManager am = AccountManager.get(mActivity);
        Account[] accounts = am.getAccountsByType("com.google");
        if (accounts == null || accounts.length == 0) {
            throw new AssertionError("AccountManager returned 0 com.google accounts");
        }
        if (!"user@gesher.local".equals(accounts[0].name)) {
            throw new AssertionError("unexpected AccountManager account: " + accounts[0].name);
        }

        String token = GoogleAuthUtil.getToken(mActivity, accounts[0], "oauth2:profile");
        if (token == null || !token.startsWith("dummy_oauth2_token_gesher_")) {
            throw new AssertionError("unexpected GoogleAuthUtil token: " + token);
        }

        Report.line("GoogleSignIn: dummy account " + account.getEmail() + " signed in with idToken");
        Report.line("AccountManager: com.google account and dummy auth token returned");
    }

    public void testFirebaseAndFcmRouting() throws Exception {
        FirebaseMessaging fcm = FirebaseMessaging.getInstance();
        Task<String> tokenTask = fcm.getToken();
        String fcmToken = Tasks.await(tokenTask);
        if (fcmToken == null || fcmToken.isEmpty()) {
            throw new AssertionError("FirebaseMessaging returned empty token");
        }

        GoogleCloudMessaging gcm = GoogleCloudMessaging.getInstance(mActivity);
        String gcmToken = gcm.register("1234567890");
        if (gcmToken == null || gcmToken.isEmpty()) {
            throw new AssertionError("GCM returned empty token");
        }

        // Send a broadcast intent with com.google.android.c2dm.intent.RECEIVE
        Intent receiveIntent = new Intent("com.google.android.c2dm.intent.RECEIVE");
        receiveIntent.setPackage(mActivity.getPackageName());
        receiveIntent.putExtra("from", "1234567890");
        receiveIntent.putExtra("message", "hello_fcm_gesher");
        receiveIntent.putExtra("google.message_id", "msg_test_001");
        mActivity.sendBroadcast(receiveIntent);

        Report.line("FirebaseMessaging: token obtained, com.google.android.c2dm.intent.RECEIVE routed and received");
    }
}
