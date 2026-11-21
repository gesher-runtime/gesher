/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.components;

import android.app.Activity;
import android.content.ContextWrapper;
import android.os.Bundle;
import android.provider.Settings;
import java.util.Objects;

/** Actual ordinary-process canWrite and provider writes, after an explicit operator mode. */
public final class SettingsAccessActivity extends Activity {
    private interface Write { void run(); }
    private static void denied(Write work) {
        try { work.run(); } catch (SecurityException expected) { return; }
        throw new AssertionError("Settings special-access denial was not enforced");
    }
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        boolean expected = getIntent().getBooleanExtra("allowed", false);
        boolean actual = Settings.System.canWrite(this);
        if (actual != expected) throw new AssertionError("Settings.System.canWrite disagrees with the live operator AppOp: " + actual);
        if (checkSelfPermission(android.Manifest.permission.WRITE_SETTINGS) != android.content.pm.PackageManager.PERMISSION_DENIED) {
            throw new AssertionError("Special access must not fabricate a signature/role grant");
        }
        String key = "gesher_settings_access_contract";
        String before = Settings.System.getString(getContentResolver(), key);
        String next = "write-" + System.nanoTime();
        if (expected) {
            if (!Settings.System.putString(getContentResolver(), key, next)
                    || !next.equals(Settings.System.getString(getContentResolver(), key))) {
                throw new AssertionError("Allowed ordinary application mutation did not reach the provider");
            }
        } else {
            denied(() -> Settings.System.putString(getContentResolver(), key, next));
            if (!Objects.equals(before, Settings.System.getString(getContentResolver(), key))) {
                throw new AssertionError("Denied ordinary application mutation changed stored state");
            }
        }
        denied(() -> Settings.Secure.putString(getContentResolver(), key, next));
        denied(() -> Settings.Global.putString(getContentResolver(), key, next));
        // canWrite uses the real process UID. A foreign claimed package cannot
        // consume another subject's AppOp or the built-in system identity.
        boolean foreignAllowed = false;
        try {
            foreignAllowed = Settings.System.canWrite(new ContextWrapper(this) {
                @Override public String getOpPackageName() { return "android"; }
            });
        } catch (SecurityException expectedDenial) { }
        if (foreignAllowed) throw new AssertionError("Foreign package claim bypassed the real process UID");
        Report.line("settings special access canWrite " + actual + " uid " + android.os.Process.myUid()
                + " pid " + android.os.Process.myPid() + "; secure/global and foreign package denied");
        finish();
    }
}
