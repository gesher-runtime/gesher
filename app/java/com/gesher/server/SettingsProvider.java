/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ContentProviderHolder;
import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.pm.ApplicationInfo;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Process;
import android.provider.Settings;

import java.util.HashMap;
import java.util.Map;

/**
 * The {@code settings} provider, as a stand-in for SettingsProvider: the
 * {@code Settings.System}, {@code Secure} and {@code Global} tables in memory, with the
 * values a freshly set up device has for the settings apps and the framework read most,
 * answered through the call() fast path Settings' NameValueCache uses ({@code GET_system},
 * {@code PUT_secure}...), and query() for the rest. Values put are kept until the system
 * server exits. No generation tracking: every read asks the provider.
 *
 * <p>It is a framework ContentProvider in the system server, so its Binder is the
 * framework's own transport ({@code ContentProvider.Transport}), served across processes.
 */
final class SettingsProvider extends ContentProvider {
    static final String AUTHORITY = Settings.AUTHORITY;

    private static final String SYSTEM = "system";
    private static final String SECURE = "secure";
    private static final String GLOBAL = "global";

    // Guarded by this. Table name -> setting -> value.
    private final Map<String, Map<String, String>> mTables = new HashMap<>();
    private final ProviderInfo mInfo = new ProviderInfo();

    SettingsProvider() {
        for (String table : new String[] {SYSTEM, SECURE, GLOBAL}) {
            mTables.put(table, new HashMap<>());
        }
        Map<String, String> system = mTables.get(SYSTEM);
        system.put(Settings.System.FONT_SCALE, "1.0");
        system.put(Settings.System.TIME_12_24,  // Windows' time format (rtl track)
                com.gesher.app.locale.SystemLocale.is24Hour() ? "24" : "12");
        system.put(Settings.System.SCREEN_OFF_TIMEOUT, "60000");
        system.put(Settings.System.SCREEN_BRIGHTNESS, "128");
        system.put(Settings.System.HAPTIC_FEEDBACK_ENABLED, "0");
        system.put(Settings.System.SOUND_EFFECTS_ENABLED, "0");
        system.put(Settings.System.ACCELEROMETER_ROTATION, "0");
        system.put(Settings.System.TEXT_AUTO_CAPS, "1");
        system.put(Settings.System.TEXT_AUTO_PUNCTUATE, "1");
        system.put(Settings.System.TEXT_SHOW_PASSWORD, "1");
        Map<String, String> secure = mTables.get(SECURE);
        secure.put(Settings.Secure.ANDROID_ID, "6765736865720000");  // "gesher" in hex
        secure.put(Settings.Secure.USER_SETUP_COMPLETE, "1");
        secure.put(Settings.Secure.ACCESSIBILITY_ENABLED, "0");
        secure.put(Settings.Secure.LOCATION_MODE, "0");
        secure.put(Settings.Secure.SHOW_IME_WITH_HARD_KEYBOARD, "0");
        secure.put("ui_night_mode", "1");
        Map<String, String> global = mTables.get(GLOBAL);
        global.put(Settings.Global.DEVICE_PROVISIONED, "1");
        global.put(Settings.Global.AUTO_TIME, "1");
        global.put(Settings.Global.AUTO_TIME_ZONE, "1");
        global.put(Settings.Global.AIRPLANE_MODE_ON, "0");
        global.put(Settings.Global.ADB_ENABLED, "0");
        global.put(Settings.Global.DEVELOPMENT_SETTINGS_ENABLED, "0");
        global.put(Settings.Global.ALWAYS_FINISH_ACTIVITIES, "0");
        global.put(Settings.Global.ANIMATOR_DURATION_SCALE, "1.0");
        global.put(Settings.Global.TRANSITION_ANIMATION_SCALE, "1.0");
        global.put(Settings.Global.WINDOW_ANIMATION_SCALE, "1.0");
        global.put(Settings.Global.STAY_ON_WHILE_PLUGGED_IN, "0");
        global.put(Settings.Global.WIFI_ON, "0");
        global.put(Settings.Global.BLUETOOTH_ON, "0");
        global.put(Settings.Global.DATA_ROAMING, "0");
        global.put(Settings.Global.DEVICE_NAME, "Gesher");

        ApplicationInfo ai = new ApplicationInfo();
        ai.packageName = "com.android.providers.settings";
        ai.processName = "system";
        ai.uid = Process.SYSTEM_UID;
        ai.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_PERSISTENT;
        ai.enabled = true;
        mInfo.applicationInfo = ai;
        mInfo.packageName = ai.packageName;
        mInfo.name = "com.android.providers.settings.SettingsProvider";
        mInfo.authority = AUTHORITY;
        mInfo.processName = ai.processName;
        mInfo.exported = true;
        mInfo.enabled = true;
        mInfo.multiprocess = false;
        mInfo.flags = ProviderInfo.FLAG_SINGLE_USER;
        // ContentProvider.attachInfo, without a Context (the transport's call() needs only
        // the authority).
        attachInfo(null, mInfo);
    }

    /** What getContentProvider answers for {@code settings}. */
    ContentProviderHolder holder() {
        ContentProviderHolder h = new ContentProviderHolder(mInfo);
        h.provider = getIContentProvider();
        h.noReleaseNeeded = true;  // the provider lives as long as the system server
        return h;
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    /** The table of a call method: GET_system → system. */
    private static String table(String method) {
        int i = method.indexOf('_');
        return i < 0 ? null : method.substring(i + 1);
    }

    @Override
    public Bundle call(String method, String arg, Bundle extras) {
        String table = table(method);
        Map<String, String> values;
        synchronized (this) {
            values = table != null ? mTables.get(table) : null;
        }
        if (values == null) {
            // GET_config and others: DeviceConfig is a stand-in of its own.
            return method.startsWith("GET_") ? valueBundle(null) : null;
        }
        if (method.startsWith("GET_")) {
            synchronized (this) {
                return valueBundle(values.get(arg));
            }
        }
        if (method.startsWith("PUT_")) {
            String value = extras != null ? extras.getString(Settings.NameValueTable.VALUE)
                    : null;
            synchronized (this) {
                if (value == null) values.remove(arg);
                else values.put(arg, value);
            }
            SystemServer.log("settings: " + table + " " + arg + " = " + value);
            return new Bundle();
        }
        if (method.startsWith("DELETE_")) {
            synchronized (this) {
                values.remove(arg);
            }
            return new Bundle();
        }
        if (method.startsWith("LIST_")) {
            java.util.ArrayList<String> all = new java.util.ArrayList<>();
            synchronized (this) {
                for (Map.Entry<String, String> e : values.entrySet()) {
                    all.add(e.getKey());
                    all.add(e.getValue());
                }
            }
            Bundle b = new Bundle();
            b.putStringArrayList(Settings.NameValueTable.VALUE, all);
            return b;
        }
        return null;
    }

    private static Bundle valueBundle(String value) {
        Bundle b = new Bundle();
        b.putString(Settings.NameValueTable.VALUE, value);
        return b;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
            String[] selectionArgs, String sortOrder) {
        // content://settings/<table>[/<name>]: the rows (name, value).
        String table = uri.getPathSegments().isEmpty() ? null : uri.getPathSegments().get(0);
        MatrixCursor c = new MatrixCursor(new String[] {Settings.NameValueTable.NAME,
                Settings.NameValueTable.VALUE});
        synchronized (this) {
            Map<String, String> values = table != null ? mTables.get(table) : null;
            if (values == null) return c;
            String only = uri.getPathSegments().size() > 1 ? uri.getLastPathSegment()
                    : selectionArgs != null && selectionArgs.length > 0 ? selectionArgs[0]
                    : null;
            for (Map.Entry<String, String> e : values.entrySet()) {
                if (only == null || only.equals(e.getKey())) {
                    c.addRow(new Object[] {e.getKey(), e.getValue()});
                }
            }
        }
        return c;
    }

    @Override
    public String getType(Uri uri) {
        return null;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        return 0;
    }
}
