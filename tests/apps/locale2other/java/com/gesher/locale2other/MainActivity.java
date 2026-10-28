/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.locale2other;

import android.app.Activity;
import android.app.ILocaleManager;
import android.app.LocaleConfig;
import android.app.LocaleManager;
import android.os.Bundle;
import android.os.LocaleList;
import android.os.ServiceManager;

/** A distinct, actually attached device process with no privileged permissions. */
public final class MainActivity extends Activity {
    private static final String TARGET = "com.gesher.locale2app";
    interface Operation { void run() throws Exception; }
    static void deny(String phase, Operation operation) throws Exception {
        try {
            operation.run();
        } catch (SecurityException expected) {
            System.out.println("locale2other: ASSERT " + phase + " SecurityException");
            return;
        }
        throw new AssertionError(phase + " accepted");
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        try {
            LocaleManager manager = getSystemService(LocaleManager.class);
            ILocaleManager service = ILocaleManager.Stub.asInterface(ServiceManager.getService("locale"));
            // Supported configs are public in the same user; selected locales are private.
            LocaleConfig config = service.getOverrideLocaleConfig(TARGET, 0);
            if (config == null || !"he-IL,fr-FR".equals(
                    config.getSupportedLocales().toLanguageTags())) {
                throw new AssertionError("same-user public override: " + config);
            }
            System.out.println("locale2other: ASSERT public override [he-IL,fr-FR]");
            deny("private selected get", () -> manager.getApplicationLocales(TARGET));
            // The public overload marks fromDelegate=true; this is not an authority bypass.
            deny("delegated selected set", () -> manager.setApplicationLocales(TARGET,
                    LocaleList.forLanguageTags("fr-FR")));
            deny("override set", () -> service.setOverrideLocaleConfig(TARGET, 0,
                    new LocaleConfig(LocaleList.forLanguageTags("fr-FR"))));
            deny("override clear", () -> service.setOverrideLocaleConfig(TARGET, 0, null));
            deny("public config cross-user", () -> service.getOverrideLocaleConfig(TARGET, 1));
            manager.setOverrideLocaleConfig(new LocaleConfig(LocaleList.forLanguageTags("he-IL")));
            manager.setApplicationLocales(LocaleList.forLanguageTags("he-IL"));
            if (!"he-IL".equals(manager.getApplicationLocales().toLanguageTags())) {
                throw new AssertionError("distinct package cannot set its own locales");
            }
            manager.setOverrideLocaleConfig(null);
            if (!manager.getApplicationLocales().isEmpty() || manager.getOverrideLocaleConfig() != null
                    || new LocaleConfig(this).getStatus() != LocaleConfig.STATUS_NOT_SPECIFIED) {
                throw new AssertionError("clear without a resource LocaleConfig did not reset selection");
            }
            if (!"he-IL,fr-FR".equals(service.getOverrideLocaleConfig(TARGET, 0)
                    .getSupportedLocales().toLanguageTags())) {
                throw new AssertionError("distinct package changed target override");
            }
            System.out.println("locale2other: ASSERT own override clear with no resource config resets selection; target unchanged");
            System.out.println("locale2other: PASS distinct registered package boundaries");
        } catch (Throwable error) {
            System.out.println("locale2other: FAIL " + error);
            error.printStackTrace();
        }
        System.out.flush();
    }
    @Override public void onResume() {
        super.onResume();
        new android.os.Handler(getMainLooper()).post(this::finish);
    }
}
