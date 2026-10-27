/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.locale2app;

import android.app.Activity;
import android.app.ILocaleManager;
import android.app.LocaleConfig;
import android.app.LocaleManager;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.Handler;
import android.os.LocaleList;
import android.os.Looper;
import android.os.Process;
import android.os.ServiceManager;
import android.os.UserHandle;

import java.util.Objects;

/** Actual framework API calls and resources; each operation runs in a real app process. */
public class MainActivity extends Activity {
    protected LocaleManager manager;
    private int callbacks;

    static void line(String text) {
        System.out.println("locale2: " + text);
        System.out.flush();
    }

    static LocaleList list(String tags) { return LocaleList.forLanguageTags(tags); }

    static void equal(String phase, Object expected, Object actual) {
        if (!Objects.equals(expected, actual)) {
            throw new AssertionError(phase + " expected=[" + expected + "] actual=[" + actual + "]");
        }
        line("ASSERT " + phase + " actual=[" + actual + "]");
    }

    interface Operation { void run() throws Exception; }

    static void rejects(String phase, Class<? extends Throwable> type, Operation operation)
            throws Exception {
        try {
            operation.run();
        } catch (Throwable error) {
            if (!type.isInstance(error)) throw new AssertionError(phase + ": " + error, error);
            line("ASSERT " + phase + " rejected=" + error.getClass().getSimpleName());
            return;
        }
        throw new AssertionError(phase + " accepted");
    }

    protected void runCase() throws Exception { }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        manager = getSystemService(LocaleManager.class);
        try {
            if (manager == null) throw new AssertionError("LocaleManager absent");
            runCase();
            line("PASS " + getClass().getSimpleName());
        } catch (Throwable error) {
            line("FAIL " + getClass().getSimpleName() + " " + error);
            error.printStackTrace();
        }
    }

    @Override public void onResume() {
        super.onResume();
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            try {
                LocaleConfig override = manager.getOverrideLocaleConfig();
                LocaleConfig effective = new LocaleConfig(this);
                LocaleConfig original = LocaleConfig.fromContextIgnoringOverride(this);
                line("STATE override=" + (override == null ? "none" : "["
                        + override.getSupportedLocales().toLanguageTags() + "]")
                        + " selected=[" + manager.getApplicationLocales().toLanguageTags() + "]"
                        + " effective=[" + effective.getSupportedLocales().toLanguageTags() + "]"
                        + " original=[" + original.getSupportedLocales().toLanguageTags() + "]"
                        + " system=[" + manager.getSystemLocales().toLanguageTags() + "]"
                        + " config=[" + getResources().getConfiguration().getLocales().toLanguageTags() + "]"
                        + " default=[" + LocaleList.getDefault().toLanguageTags() + "]"
                        + " greeting=" + getString(R.string.greeting)
                        + " callbacks=" + callbacks + " pid=" + Process.myPid());
            } catch (Throwable error) {
                line("FAIL report " + error);
            } finally {
                line("DONE " + getClass().getSimpleName());
                finish();
            }
        }, 600);
    }

    @Override public void onConfigurationChanged(Configuration config) {
        super.onConfigurationChanged(config);
        callbacks++;
        line("CALLBACK " + callbacks + " locales=[" + config.getLocales().toLanguageTags()
                + "] greeting=" + getString(R.string.greeting));
    }

    void check(String override, String selected) {
        LocaleConfig config = manager.getOverrideLocaleConfig();
        equal("override", override, config == null ? null
                : config.getSupportedLocales().toLanguageTags());
        if (config != null) equal("override status", LocaleConfig.STATUS_SUCCESS, config.getStatus());
        equal("selected", selected, manager.getApplicationLocales().toLanguageTags());
    }

    public static final class Seed extends MainActivity {
        @Override protected void runCase() {
            manager.setOverrideLocaleConfig(null);
            manager.setApplicationLocales(list("fr-CA,zh-Hant-TW,he-IL,en-GB,zh-Hans-CN"));
            manager.setOverrideLocaleConfig(new LocaleConfig(list("fr-FR,zh-Hant,en-US")));
            check("fr-FR,zh-Hant,en-US", "fr-CA,zh-Hant-TW,en-GB");
        }
    }

    public static final class Equal extends MainActivity {
        @Override protected void runCase() {
            manager.setApplicationLocales(list("he-IL"));
            manager.setOverrideLocaleConfig(new LocaleConfig(list("en-US,fr-FR,zh-Hant")));
            // Pinned isSameLocaleConfig ignores ordering and returns before reconciliation.
            check("fr-FR,zh-Hant,en-US", "he-IL");
        }
    }

    public static final class Replace extends MainActivity {
        @Override protected void runCase() {
            manager.setApplicationLocales(list("he-IL,fr-CA,zh-Hant-TW,en-GB"));
            manager.setOverrideLocaleConfig(new LocaleConfig(list("he-IL,fr-FR")));
            check("he-IL,fr-FR", "he-IL,fr-CA");
        }
    }

    public static final class Clear extends MainActivity {
        @Override protected void runCase() {
            manager.setOverrideLocaleConfig(new LocaleConfig(list("de-DE,fr-FR")));
            manager.setApplicationLocales(list("de-DE,fr-CA"));
            manager.setOverrideLocaleConfig(null);
            check(null, "fr-CA");
            equal("clear original config", "en-US,he-IL,fr-FR",
                    new LocaleConfig(this).getSupportedLocales().toLanguageTags());
        }
    }

    public static final class Empty extends MainActivity {
        @Override protected void runCase() {
            manager.setApplicationLocales(list("he-IL"));
            manager.setOverrideLocaleConfig(new LocaleConfig(LocaleList.getEmptyLocaleList()));
            check("", "");
        }
    }

    public static final class NullList extends MainActivity {
        @Override protected void runCase() {
            manager.setOverrideLocaleConfig(new LocaleConfig(list("he-IL")));
            manager.setApplicationLocales(list("he-IL"));
            manager.setOverrideLocaleConfig(new LocaleConfig((LocaleList) null));
            check("", "");
        }
    }

    public static final class FailureSeed extends MainActivity {
        @Override protected void runCase() {
            manager.setOverrideLocaleConfig(new LocaleConfig(list("he-IL,fr-FR")));
            manager.setApplicationLocales(list("he-IL"));
            check("he-IL,fr-FR", "he-IL");
        }
    }

    public static final class Failure extends MainActivity {
        @Override protected void runCase() {
            // CI blocks the real staging path or rename target, not a model/store substitute.
            manager.setOverrideLocaleConfig(new LocaleConfig(list("fr-FR")));
            check("he-IL,fr-FR", "he-IL");
        }
    }

    public static final class Bounds extends MainActivity {
        @Override protected void runCase() throws Exception {
            String own = getPackageName();
            ILocaleManager service = ILocaleManager.Stub.asInterface(ServiceManager.getService("locale"));
            rejects("selected null package", NullPointerException.class,
                    () -> service.getApplicationLocales(null, 0));
            rejects("selected null list", NullPointerException.class,
                    () -> service.setApplicationLocales(own, 0, null, false));
            rejects("override null package get", NullPointerException.class,
                    () -> service.getOverrideLocaleConfig(null, 0));
            rejects("override null package set", NullPointerException.class,
                    () -> service.setOverrideLocaleConfig(null, 0, null));
            rejects("selected unknown package", IllegalArgumentException.class,
                    () -> manager.getApplicationLocales("com.gesher.locale2missing"));
            rejects("set unknown package", IllegalArgumentException.class,
                    () -> manager.setApplicationLocales("com.gesher.locale2missing", list("fr-FR")));
            rejects("override unknown package", IllegalArgumentException.class,
                    () -> service.setOverrideLocaleConfig("com.gesher.locale2missing", 0, null));
            equal("public override unknown", null,
                    service.getOverrideLocaleConfig("com.gesher.locale2missing", 0));
            for (int user : new int[] {1, UserHandle.USER_CURRENT, UserHandle.USER_ALL}) {
                rejects("selected read user " + user, SecurityException.class,
                        () -> service.getApplicationLocales(own, user));
                rejects("selected write user " + user, SecurityException.class,
                        () -> service.setApplicationLocales(own, user, list("fr-FR"), true));
                rejects("override read user " + user, SecurityException.class,
                        () -> service.getOverrideLocaleConfig(own, user));
                rejects("override write user " + user, SecurityException.class,
                        () -> service.setOverrideLocaleConfig(own, user, null));
            }
            equal("current or self", manager.getApplicationLocales().toLanguageTags(),
                    service.getApplicationLocales(own, UserHandle.USER_CURRENT_OR_SELF).toLanguageTags());
        }
    }
}
