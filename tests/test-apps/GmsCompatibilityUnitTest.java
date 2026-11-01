/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.tests;

import android.accounts.Account;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.location.Location;
import android.location.LocationManager;
import android.os.Bundle;

import com.gesher.server.GmsPackageSupport;
import com.google.android.gms.auth.GoogleAuthUtil;
import com.google.android.gms.auth.api.signin.GoogleSignInAccount;
import com.google.android.gms.auth.api.signin.GoogleSignInOptions;
import com.google.android.gms.auth.api.signin.GoogleSignInStatusCodes;
import com.google.android.gms.common.ConnectionResult;
import com.google.android.gms.common.GoogleApiAvailabilityLight;
import com.google.android.gms.common.api.Scope;
import com.google.android.gms.location.FusedLocationProviderClient;
import com.google.android.gms.location.LocationCallback;
import com.google.android.gms.location.LocationRequest;
import com.google.android.gms.location.LocationResult;
import com.google.android.gms.tasks.Task;
import com.google.android.gms.tasks.TaskCompletionSource;
import com.google.android.gms.tasks.Tasks;
import com.google.firebase.messaging.FirebaseMessaging;
import com.google.firebase.messaging.RemoteMessage;

import java.util.Collections;
import java.util.Map;
import java.util.concurrent.ExecutionException;

/**
 * Standalone unit test suite for Gesher's GMS & microG compatibility layer.
 */
public final class GmsCompatibilityUnitTest {
    public static void main(String[] args) throws Exception {
        System.out.println("Running GmsCompatibilityUnitTest...");
        testTasksApi();
        testConnectionResult();
        testGoogleApiAvailability();
        testGoogleSignInAccountAndOptions();
        testGoogleAuthUtil();
        testFirebaseMessagingAndRemoteMessage();
        testFusedLocationProviderClient();
        testGmsPackageSupport();
        System.out.println("ALL GMS COMPATIBILITY UNIT TESTS PASSED");
    }

    private static void assertEquals(Object expected, Object actual, String message) {
        if (expected == null && actual == null) return;
        if (expected != null && expected.equals(actual)) return;
        throw new AssertionError(message + " - expected: " + expected + ", actual: " + actual);
    }

    private static void assertTrue(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void testTasksApi() throws Exception {
        Task<String> task = Tasks.forResult("hello_gms");
        assertTrue(task.isComplete(), "Task should be complete");
        assertTrue(task.isSuccessful(), "Task should be successful");
        assertEquals("hello_gms", Tasks.await(task), "Task result mismatch");

        TaskCompletionSource<Integer> tcs = new TaskCompletionSource<>();
        Task<Integer> asyncTask = tcs.getTask();
        assertTrue(!asyncTask.isComplete(), "Task should not be complete initially");
        tcs.setResult(42);
        assertTrue(asyncTask.isComplete(), "Task should be complete after setResult");
        assertEquals(Integer.valueOf(42), Tasks.await(asyncTask), "TaskCompletionSource result mismatch");

        final boolean[] successRan = new boolean[1];
        asyncTask.addOnSuccessListener(res -> successRan[0] = (res == 42));
        assertTrue(successRan[0], "OnSuccessListener should have executed");

        System.out.println("PASS: testTasksApi");
    }

    private static void testConnectionResult() {
        ConnectionResult res = ConnectionResult.RESULT_SUCCESS;
        assertTrue(res.isSuccess(), "RESULT_SUCCESS should be success");
        assertEquals(ConnectionResult.SUCCESS, res.getErrorCode(), "Error code should be 0");
        assertTrue(!res.hasResolution(), "RESULT_SUCCESS should not have resolution");

        ConnectionResult missing = new ConnectionResult(ConnectionResult.SERVICE_MISSING);
        assertTrue(!missing.isSuccess(), "SERVICE_MISSING should not be success");
        assertEquals(ConnectionResult.SERVICE_MISSING, missing.getErrorCode(), "Error code mismatch");

        System.out.println("PASS: testConnectionResult");
    }

    private static void testGoogleApiAvailability() {
        GoogleApiAvailabilityLight light = GoogleApiAvailabilityLight.getInstance();
        assertEquals(ConnectionResult.SUCCESS, light.isGooglePlayServicesAvailable(null),
                "isGooglePlayServicesAvailable should return SUCCESS");
        assertEquals(ConnectionResult.SUCCESS, light.isGooglePlayServicesAvailable(null, 200000000),
                "isGooglePlayServicesAvailable with minVersion should return SUCCESS");
        assertEquals("SUCCESS", light.getErrorString(ConnectionResult.SUCCESS), "Error string mismatch");
        assertTrue(!light.isUserResolvableError(ConnectionResult.SUCCESS),
                "SUCCESS should not be user resolvable error");

        System.out.println("PASS: testGoogleApiAvailability");
    }

    private static void testGoogleSignInAccountAndOptions() {
        GoogleSignInAccount account = GoogleSignInAccount.createDefault();
        assertEquals("user@gesher.local", account.getEmail(), "Account email mismatch");
        assertEquals("Gesher User", account.getDisplayName(), "Account display name mismatch");
        assertTrue(account.getIdToken() != null && !account.getIdToken().isEmpty(), "idToken should not be empty");
        assertTrue(account.getServerAuthCode() != null, "serverAuthCode should not be null");
        assertTrue(!account.isExpired(), "Account should not be expired");

        GoogleSignInOptions options = new GoogleSignInOptions.Builder(GoogleSignInOptions.DEFAULT_SIGN_IN)
                .requestEmail()
                .requestIdToken("dummy_client_id")
                .build();
        assertTrue(options.isIdTokenRequested(), "ID token should be requested");
        assertEquals("dummy_client_id", options.getServerClientId(), "Server client ID mismatch");

        assertEquals("SIGN_IN_SUCCESS", GoogleSignInStatusCodes.getStatusCodeString(GoogleSignInStatusCodes.SIGN_IN_SUCCESS),
                "Status code string mismatch");

        System.out.println("PASS: testGoogleSignInAccountAndOptions");
    }

    private static void testGoogleAuthUtil() throws Exception {
        Account acc = new Account("user@gesher.local", "com.google");
        String token = GoogleAuthUtil.getToken(null, acc, "oauth2:profile");
        assertTrue(token != null && token.startsWith("dummy_oauth2_token_gesher_"),
                "GoogleAuthUtil token format mismatch: " + token);

        System.out.println("PASS: testGoogleAuthUtil");
    }

    private static void testFirebaseMessagingAndRemoteMessage() throws Exception {
        FirebaseMessaging fcm = FirebaseMessaging.getInstance();
        Task<String> tokenTask = fcm.getToken();
        String token = Tasks.await(tokenTask);
        assertTrue(token != null && token.startsWith("fcm_token_gesher_"), "FCM token mismatch: " + token);

        Bundle b = new Bundle();
        b.putString("from", "1234567890");
        b.putString("google.message_id", "msg_unit_123");
        b.putString("title", "Test Title");
        b.putString("data_key", "data_val");

        RemoteMessage msg = new RemoteMessage(b);
        assertEquals("1234567890", msg.getFrom(), "RemoteMessage from mismatch");
        assertEquals("msg_unit_123", msg.getMessageId(), "RemoteMessage messageId mismatch");
        Map<String, String> data = msg.getData();
        assertEquals("data_val", data.get("data_key"), "RemoteMessage data mismatch");

        System.out.println("PASS: testFirebaseMessagingAndRemoteMessage");
    }

    private static void testFusedLocationProviderClient() throws Exception {
        Context ctx = new Context();
        LocationManager lm = (LocationManager) ctx.getSystemService(Context.LOCATION_SERVICE);
        Location testLoc = new Location(LocationManager.FUSED_PROVIDER);
        testLoc.setLatitude(31.7767);
        testLoc.setLongitude(35.2345);
        testLoc.setAccuracy(5.0f);
        lm.setLastKnownLocationForTest(testLoc);

        FusedLocationProviderClient client = new FusedLocationProviderClient(ctx);
        Task<Location> locTask = client.getLastLocation();
        Location loc = Tasks.await(locTask);
        assertTrue(loc != null, "getLastLocation should return location");
        assertEquals(31.7767, loc.getLatitude(), "latitude mismatch");

        LocationCallback cb = new LocationCallback() {
            @Override
            public void onLocationResult(LocationResult locationResult) {
            }
        };
        LocationRequest req = LocationRequest.create().setInterval(1000);
        Task<Void> reqTask = client.requestLocationUpdates(req, cb, null);
        Tasks.await(reqTask);
        assertTrue(reqTask.isSuccessful(), "requestLocationUpdates should succeed");

        Task<Void> rmTask = client.removeLocationUpdates(cb);
        Tasks.await(rmTask);
        assertTrue(rmTask.isSuccessful(), "removeLocationUpdates should succeed");

        System.out.println("PASS: testFusedLocationProviderClient");
    }

    private static void testGmsPackageSupport() {
        assertTrue(GmsPackageSupport.isSyntheticPackage("com.google.android.gms"), "isSyntheticPackage GMS");
        assertTrue(GmsPackageSupport.isSyntheticPackage("com.android.vending"), "isSyntheticPackage Vending");
        assertTrue(!GmsPackageSupport.isSyntheticPackage("com.example.app"), "isSyntheticPackage negative");

        PackageInfo gmsInfo = GmsPackageSupport.createSyntheticPackageInfo(
                GmsPackageSupport.PACKAGE_GMS, PackageManager.GET_SIGNATURES);
        assertTrue(gmsInfo != null, "gmsInfo not null");
        assertEquals("com.google.android.gms", gmsInfo.packageName, "gmsInfo packageName");
        assertEquals(233013000, gmsInfo.versionCode, "gmsInfo versionCode");
        assertTrue(gmsInfo.signatures != null && gmsInfo.signatures.length > 0, "gmsInfo signatures");
        assertTrue(gmsInfo.applicationInfo != null && gmsInfo.applicationInfo.enabled, "gmsInfo applicationInfo enabled");

        PackageInfo vendingInfo = GmsPackageSupport.createSyntheticPackageInfo(
                GmsPackageSupport.PACKAGE_VENDING, PackageManager.GET_SIGNATURES);
        assertTrue(vendingInfo != null, "vendingInfo not null");
        assertEquals("com.android.vending", vendingInfo.packageName, "vendingInfo packageName");
        assertEquals(83000000, vendingInfo.versionCode, "vendingInfo versionCode");
        assertTrue(vendingInfo.signatures != null && vendingInfo.signatures.length > 0, "vendingInfo signatures");

        System.out.println("PASS: testGmsPackageSupport");
    }
}
