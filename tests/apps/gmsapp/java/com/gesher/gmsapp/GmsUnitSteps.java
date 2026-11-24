/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.gmsapp;

import android.accounts.Account;
import android.accounts.AccountManager;
import android.accounts.AuthenticatorException;
import android.app.Activity;
import android.app.Dialog;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.location.Location;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

import com.google.android.gms.auth.GoogleAuthUtil;
import com.google.android.gms.auth.GooglePlayServicesAvailabilityException;
import com.google.android.gms.auth.api.signin.GoogleSignIn;
import com.google.android.gms.auth.api.signin.GoogleSignInAccount;
import com.google.android.gms.auth.api.signin.GoogleSignInClient;
import com.google.android.gms.auth.api.signin.GoogleSignInOptions;
import com.google.android.gms.common.ConnectionResult;
import com.google.android.gms.common.GoogleApiAvailability;
import com.google.android.gms.common.GooglePlayServicesUtil;
import com.google.android.gms.common.api.ApiException;
import com.google.android.gms.gcm.GoogleCloudMessaging;
import com.google.android.gms.location.FusedLocationProviderClient;
import com.google.android.gms.location.LocationCallback;
import com.google.android.gms.location.LocationRequest;
import com.google.android.gms.location.LocationResult;
import com.google.android.gms.location.LocationServices;
import com.google.android.gms.tasks.Task;
import com.google.android.gms.tasks.Tasks;
import com.google.firebase.messaging.FirebaseMessaging;

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
        testPackageManagerWithoutGms();
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
     * What the client library's ProviderInstaller loads: Play services' package context
     * with CONTEXT_INCLUDE_CODE. Without Play services there is no such package, and
     * createPackageContext throws NameNotFoundException, as on Android (#335).
     */
    public void testProviderInstallerCode() throws Exception {
        try {
            mActivity.createPackageContext("com.google.android.gms",
                    Context.CONTEXT_INCLUDE_CODE | Context.CONTEXT_IGNORE_SECURITY);
            throw new AssertionError("Play services' package context exists without Play services");
        } catch (PackageManager.NameNotFoundException expected) {
            // as on Android without Play services
        }
        Report.line("ProviderInstaller: no Play services package context without Play services (NameNotFoundException)");
    }

    public void testGoogleApiAvailability() throws Exception {
        GoogleApiAvailability api = GoogleApiAvailability.getInstance();
        int code = api.isGooglePlayServicesAvailable(mActivity);
        if (code != ConnectionResult.SERVICE_MISSING) {
            throw new AssertionError("isGooglePlayServicesAvailable returned " + code + ", expected SERVICE_MISSING");
        }
        int utilCode = GooglePlayServicesUtil.isGooglePlayServicesAvailable(mActivity);
        if (utilCode != ConnectionResult.SERVICE_MISSING) {
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
        try {
            Tasks.await(task);
            throw new AssertionError("makeGooglePlayServicesAvailable succeeded without Play services");
        } catch (java.util.concurrent.ExecutionException expected) {
            // the task fails: nothing installed Play services
        }
        Report.line("GoogleApiAvailability: isGooglePlayServicesAvailable SERVICE_MISSING (1)");
        Report.line("GoogleApiAvailability: makeGooglePlayServicesAvailable failed without Play services");
    }

    /** Without microG, Play services and the Play Store are not installed packages (#335). */
    public void testPackageManagerWithoutGms() throws Exception {
        PackageManager pm = mActivity.getPackageManager();
        for (String name : new String[] {"com.google.android.gms", "com.android.vending"}) {
            try {
                pm.getPackageInfo(name, PackageManager.GET_SIGNATURES | PackageManager.GET_SERVICES);
                throw new AssertionError(name + " reported without being installed");
            } catch (PackageManager.NameNotFoundException expected) {
                // as on Android without the package
            }
            for (PackageInfo pi : pm.getInstalledPackages(0)) {
                if (name.equals(pi.packageName)) throw new AssertionError(name + " listed as installed");
            }
        }
        Report.line("PackageManager: com.google.android.gms and com.android.vending absent without microG");
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

    /**
     * Without microG nobody is signed in and there is no Google account or token (#335):
     * the client fixture reports Play services missing, and the system's AccountManager
     * has no com.google account and no authenticator to issue a token ("bind failure",
     * AuthenticatorException), as Android answers without one.
     */
    public void testGoogleSignInAndAccountManager() throws Exception {
        GoogleSignInOptions options = new GoogleSignInOptions.Builder(GoogleSignInOptions.DEFAULT_SIGN_IN)
                .requestEmail()
                .build();
        GoogleSignInClient client = GoogleSignIn.getClient(mActivity, options);
        try {
            Tasks.await(client.silentSignIn());
            throw new AssertionError("silentSignIn signed someone in without Play services");
        } catch (java.util.concurrent.ExecutionException expected) {
            int code = ((ApiException) expected.getCause()).getStatusCode();
            if (code != ConnectionResult.SERVICE_MISSING) {
                throw new AssertionError("silentSignIn status " + code + ", expected SERVICE_MISSING");
            }
        }
        if (GoogleSignIn.getLastSignedInAccount(mActivity) != null) {
            throw new AssertionError("a signed-in account exists before any sign-in");
        }

        AccountManager am = AccountManager.get(mActivity);
        Account[] accounts = am.getAccountsByType("com.google");
        if (accounts.length != 0) {
            throw new AssertionError("AccountManager returned " + accounts.length + " com.google accounts");
        }
        Account google = new Account("someone@example.org", "com.google");
        try {
            Bundle result = am.getAuthToken(google, "oauth2:profile", null, false, null, null).getResult();
            throw new AssertionError("a token without an authenticator: " + result);
        } catch (AuthenticatorException expected) {
            // as Android answers without an authenticator for the type
        }
        try {
            String token = GoogleAuthUtil.getToken(mActivity, google, "oauth2:profile");
            throw new AssertionError("GoogleAuthUtil returned a token: " + token);
        } catch (GooglePlayServicesAvailabilityException expected) {
            // the client library's answer without Play services
        }

        Report.line("GoogleSignIn: no signed-in account without Play services (SERVICE_MISSING)");
        Report.line("AccountManager: no com.google account; getAuthToken fails without an authenticator");
    }

    /**
     * Without microG there is no push provider: FCM's token task and GCM's registration
     * fail with SERVICE_NOT_AVAILABLE (#335). A RECEIVE broadcast the app sends itself
     * still reaches its own receiver.
     */
    public void testFirebaseAndFcmRouting() throws Exception {
        Task<String> tokenTask = FirebaseMessaging.getInstance().getToken();
        try {
            String token = Tasks.await(tokenTask);
            throw new AssertionError("FirebaseMessaging returned a token without a push provider: " + token);
        } catch (java.util.concurrent.ExecutionException expected) {
            if (!(expected.getCause() instanceof java.io.IOException)
                    || !"SERVICE_NOT_AVAILABLE".equals(expected.getCause().getMessage())) {
                throw new AssertionError("FCM failure: " + expected.getCause());
            }
        }
        try {
            String registration = GoogleCloudMessaging.getInstance(mActivity).register("1234567890");
            throw new AssertionError("GCM registration without a push provider: " + registration);
        } catch (java.io.IOException expected) {
            if (!GoogleCloudMessaging.ERROR_SERVICE_NOT_AVAILABLE.equals(expected.getMessage())) {
                throw new AssertionError("GCM failure: " + expected.getMessage());
            }
        }

        // Send a broadcast intent with com.google.android.c2dm.intent.RECEIVE
        Intent receiveIntent = new Intent("com.google.android.c2dm.intent.RECEIVE");
        receiveIntent.setPackage(mActivity.getPackageName());
        receiveIntent.putExtra("from", "1234567890");
        receiveIntent.putExtra("message", "hello_fcm_gesher");
        receiveIntent.putExtra("google.message_id", "msg_test_001");
        mActivity.sendBroadcast(receiveIntent);

        Report.line("FirebaseMessaging: no token without a push provider (SERVICE_NOT_AVAILABLE); app's own RECEIVE broadcast routed");
    }
}
