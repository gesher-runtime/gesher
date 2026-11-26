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

import com.google.android.gms.auth.GoogleAuthUtil;
import com.google.android.gms.auth.GooglePlayServicesAvailabilityException;
import com.google.android.gms.auth.api.signin.GoogleSignIn;
import com.google.android.gms.auth.api.signin.GoogleSignInAccount;
import com.google.android.gms.auth.api.signin.GoogleSignInOptions;
import com.google.android.gms.auth.api.signin.GoogleSignInStatusCodes;
import com.google.android.gms.common.ConnectionResult;
import com.google.android.gms.common.GoogleApiAvailabilityLight;
import com.google.android.gms.common.api.ApiException;
import com.google.android.gms.common.api.Scope;
import com.google.android.gms.gcm.GoogleCloudMessaging;
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
        // The host Context has no installed packages: no Play services (#335).
        Context ctx = new Context();
        assertEquals(ConnectionResult.SERVICE_MISSING, light.isGooglePlayServicesAvailable(ctx),
                "isGooglePlayServicesAvailable without Play services should return SERVICE_MISSING");
        assertEquals(ConnectionResult.SERVICE_MISSING, light.isGooglePlayServicesAvailable(ctx, 200000000),
                "isGooglePlayServicesAvailable with minVersion should return SERVICE_MISSING");
        assertEquals(0, light.getApkVersion(ctx), "no Play services version without the package");
        assertEquals("SUCCESS", light.getErrorString(ConnectionResult.SUCCESS), "Error string mismatch");
        assertTrue(!light.isUserResolvableError(ConnectionResult.SUCCESS),
                "SUCCESS should not be user resolvable error");

        System.out.println("PASS: testGoogleApiAvailability");
    }

    private static void testGoogleSignInAccountAndOptions() {
        // A value object as a sign-in result carries it; the fixture creates none itself (#335).
        GoogleSignInAccount account = new GoogleSignInAccount("1", "someone@example.org", "Someone",
                "Some", "One", null, "id-token", "auth-code", System.currentTimeMillis() / 1000 + 3600,
                Collections.singleton(new Scope("email")));
        assertEquals("someone@example.org", account.getEmail(), "Account email mismatch");
        assertEquals("Someone", account.getDisplayName(), "Account display name mismatch");
        assertTrue(!account.isExpired(), "Account should not be expired");
        assertTrue(GoogleSignIn.getLastSignedInAccount(new Context()) == null,
                "a signed-in account exists before any sign-in");
        Task<GoogleSignInAccount> silent = GoogleSignIn.getClient(new Context(),
                GoogleSignInOptions.DEFAULT_SIGN_IN).silentSignIn();
        assertTrue(silent.isComplete() && !silent.isSuccessful(), "silentSignIn succeeded without Play services");
        assertEquals(ConnectionResult.SERVICE_MISSING, ((ApiException) silent.getException()).getStatusCode(),
                "silentSignIn status without Play services");

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
        // No Play services in this host: the client library's availability error, no token.
        Account acc = new Account("someone@example.org", "com.google");
        try {
            String token = GoogleAuthUtil.getToken(new Context(), acc, "oauth2:profile");
            throw new AssertionError("GoogleAuthUtil returned a token without Play services: " + token);
        } catch (GooglePlayServicesAvailabilityException expected) {
            assertEquals(ConnectionResult.SERVICE_MISSING, expected.getConnectionStatusCode(),
                    "GoogleAuthUtil availability code");
        }

        System.out.println("PASS: testGoogleAuthUtil");
    }

    private static void testFirebaseMessagingAndRemoteMessage() throws Exception {
        // No push provider: the registration fails as Firebase's GmsRpc reports it (#335).
        FirebaseMessaging fcm = FirebaseMessaging.getInstance();
        Task<String> tokenTask = fcm.getToken();
        assertTrue(tokenTask.isComplete() && !tokenTask.isSuccessful(), "FCM token without a push provider");
        assertTrue(tokenTask.getException() instanceof java.io.IOException
                && "SERVICE_NOT_AVAILABLE".equals(tokenTask.getException().getMessage()),
                "FCM failure: " + tokenTask.getException());
        try {
            String registration = GoogleCloudMessaging.getInstance(new Context()).register("1234567890");
            throw new AssertionError("GCM registration without a push provider: " + registration);
        } catch (java.io.IOException expected) {
            assertEquals(GoogleCloudMessaging.ERROR_SERVICE_NOT_AVAILABLE, expected.getMessage(), "GCM failure");
        }

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
}
