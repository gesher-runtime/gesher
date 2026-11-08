/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.peer;

import android.app.Activity;
import android.app.ActivityOptions;
import android.app.ComponentCaller;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;

/** Real callbacks, an immutable old snapshot, relaunch, and delegated creator attribution. */
public final class CallerContractActivity extends Activity {
    private static final String SOURCE = "com.gesher.components";
    private static final Uri ITEM = Uri.parse("content://com.gesher.peer.caller/item?version=1#one");
    private static final int READ = Intent.FLAG_GRANT_READ_URI_PERMISSION;
    private static final int WRITE = Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
    private static final int REQUEST = 732;
    private static ComponentCaller initial, ordinary;
    private ComponentCaller dataResult;
    private static PendingIntent creatorShared, senderShared;
    private static int checks;
    private int phase;
    private final Handler handler = new Handler(Looper.getMainLooper());

    private static void check(boolean okay, String reason) {
        if (!okay) throw new AssertionError("ComponentCallerLifecycleTest peer: " + reason);
        checks++;
    }
    private void historical(ComponentCaller caller, int flags, int expected, String reason) {
        check(caller.checkContentUriPermission(ITEM, flags) == expected, reason);
    }
    private void sharedSource(ComponentCaller caller) {
        try {
            check(SOURCE.equals(caller.getPackage()), "Ordinary sender package lost");
            check(caller.getUid() == getPackageManager().getPackageUid(SOURCE, 0), "Ordinary sender UID lost");
        } catch (PackageManager.NameNotFoundException failure) { throw new AssertionError(failure); }
    }
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (state == null) {
            checks = 0; ordinary = null; initial = getInitialCaller();
            creatorShared = getIntent().getParcelableExtra("creatorShared", PendingIntent.class);
            senderShared = getIntent().getParcelableExtra("senderShared", PendingIntent.class);
            check(creatorShared != null && senderShared != null, "Real cross-package capabilities missing");
            check(initial != null && initial.equals(getCaller()), "Initial caller was not attached");
            sharedSource(initial);
            check(SOURCE.equals(getLaunchedFromPackage()) && getLaunchedFromUid() == initial.getUid(),
                    "Legacy initial identity disagrees with ComponentCaller");
            historical(initial, READ, PackageManager.PERMISSION_DENIED, "Initial sender had no private-provider access");
            historical(initial, WRITE, PackageManager.PERMISSION_DENIED, "Initial sender acquired write access");
            for (String path : new String[]{"stream", "clip", "nested"}) {
                check(initial.checkContentUriPermission(Uri.parse("content://com.gesher.peer.caller/" + path), READ)
                        == PackageManager.PERMISSION_DENIED, "Delivered URI location was not captured: " + path);
            }
            try {
                initial.checkContentUriPermission(Uri.parse("content://com.gesher.peer.caller/absent"), READ);
                throw new AssertionError("Absent URI accepted");
            } catch (IllegalArgumentException expected) { checks++; }
            grantUriPermission(SOURCE, ITEM, READ);
            phase = 1;
        } else {
            phase = state.getInt("phase");
            check(phase == 2 && initial != null && ordinary != null, "Unexpected relaunch phase");
            check(initial.equals(getInitialCaller()), "Relaunch replaced initial caller token");
            historical(initial, READ, PackageManager.PERMISSION_DENIED, "Relaunch upgraded historical denial");
            historical(ordinary, READ, PackageManager.PERMISSION_GRANTED, "Relaunch lost retained delivery token");
        }
    }
    @Override protected void onSaveInstanceState(Bundle state) {
        state.putInt("phase", phase);
        super.onSaveInstanceState(state);
    }
    @Override protected void onResume() {
        super.onResume();
        if (phase == 1) {
            phase = 11; // Do not start another relay when returning to this activity.
            startActivity(new Intent().setClassName(SOURCE, SOURCE + ".CallerRelayActivity"));
        } else if (phase == 2) {
            phase = 3;
            // CLEAR_TOP finishes this separate holder Activity before the
            // accepted callback reaches us. Its retirement cannot cancel it.
            startActivity(new Intent(this, CallerRelayActivity.class)
                    .putExtra("capability", creatorShared));
        }
    }
    private void send(PendingIntent capability, Bundle options) {
        try { startIntentSenderForResult(capability.getIntentSender(), -1, null, 0, 0, 0, options); }
        catch (android.content.IntentSender.SendIntentException failure) {
            throw new AssertionError("Valid creator capability canceled", failure);
        }
    }
    @Override public void onNewIntent(Intent intent, ComponentCaller caller) {
        super.onNewIntent(intent, caller);
        check(caller.equals(getCurrentCaller()), "New-intent callback/current caller mismatch");
        check(initial.equals(getInitialCaller()), "New intent replaced initial caller");
        if ("ordinary".equals(intent.getStringExtra("phase"))) {
            check(phase == 11 && !caller.equals(initial), "Ordinary delivery reused initial token");
            sharedSource(caller);
            historical(caller, READ, PackageManager.PERMISSION_GRANTED, "New delivery omitted real explicit read grant");
            historical(caller, WRITE, PackageManager.PERMISSION_DENIED, "Read grant became write access");
            historical(initial, READ, PackageManager.PERMISSION_DENIED, "New grant upgraded initial snapshot");
            check(getCaller().equals(initial), "getCaller changed before setIntent");
            setIntent(intent, caller);
            check(getCaller().equals(caller), "setIntent did not retain caller");
            ordinary = caller;
            revokeUriPermission(SOURCE, ITEM, READ);
            historical(ordinary, READ, PackageManager.PERMISSION_GRANTED, "Revocation rewrote historical snapshot");
            phase = 2;
            handler.post(this::recreate);
        } else if ("creatorShared".equals(intent.getStringExtra("phase"))) {
            check(phase == 3, "Creator-shared delivery out of order");
            sharedSource(caller);
            historical(caller, READ, PackageManager.PERMISSION_DENIED, "Holder provider authority substituted for creator");
            historical(caller, WRITE, PackageManager.PERMISSION_DENIED, "Holder write authority substituted for creator");
            historical(ordinary, WRITE, PackageManager.PERMISSION_DENIED, "Delegation upgraded previous caller");
            phase = 5;
            handler.post(() -> send(senderShared, ActivityOptions.makeBasic().setShareIdentityEnabled(true).toBundle()));
        } else if ("senderShared".equals(intent.getStringExtra("phase"))) {
            check(phase == 5, "Sender-shared delivery out of order");
            sharedSource(caller);
            historical(caller, READ, PackageManager.PERMISSION_DENIED, "Sender sharing changed creator URI authority");
            phase = 4;
            handler.post(() -> startActivityForResult(new Intent().setClassName(SOURCE,
                    SOURCE + ".CallerResultActivity"), REQUEST));
        } else throw new AssertionError("Unexpected new Intent phase");
    }
    @Override public void onActivityResult(int request, int result, Intent data, ComponentCaller caller) {
        super.onActivityResult(request, result, data, caller);
        if (phase == 6) {
            check(request == REQUEST + 1 && result == RESULT_OK && data == null, "Null-data result not delivered");
            check(caller.equals(getCurrentCaller()) && !caller.equals(dataResult), "Null result reused or lost caller token");
            try {
                caller.checkContentUriPermission(ITEM, READ);
                throw new AssertionError("Null result invented URI history");
            } catch (IllegalArgumentException expected) { checks++; }
            complete();
            return;
        }
        check(phase == 4 && request == REQUEST && result == RESULT_OK && data != null && ITEM.equals(data.getData()),
                "Result phase or data mismatch");
        check(caller.equals(getCurrentCaller()) && !caller.equals(initial) && !caller.equals(ordinary),
                "Result token was not a distinct current delivery");
        check(caller.getUid() == Process.INVALID_UID && caller.getPackage() == null,
                "Result sender identity leaked without sharing");
        historical(caller, READ, PackageManager.PERMISSION_DENIED, "Revoked result sender regained read authority");
        historical(ordinary, READ, PackageManager.PERMISSION_GRANTED, "Later result rewrote old delivery");
        check(initial.equals(getInitialCaller()), "Result replaced initial caller");
        dataResult = caller;
        phase = 6;
        handler.post(() -> startActivityForResult(new Intent().setClassName(SOURCE,
                SOURCE + ".CallerResultActivity").putExtra("nullData", true), REQUEST + 1));
    }
    private void complete() {
        grantUriPermission(SOURCE, ITEM, READ);
        setResult(RESULT_OK, new Intent().setData(ITEM).putExtra("peerChecks", checks));
        System.out.println("ComponentCallerLifecyclePeer all passed: " + checks + " checks");
        System.out.flush();
        finish();
    }
}
