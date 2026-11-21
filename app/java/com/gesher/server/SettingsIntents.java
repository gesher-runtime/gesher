/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.Intent;
import android.net.Uri;

import java.util.HashMap;
import java.util.Map;

/**
 * Android Settings screens, which Gesher has no Settings APK to show.
 *
 * <p>Apps open them with an implicit {@code android.settings.*} intent. On a phone the
 * Settings app always handles them, so many apps call {@code startActivity} without
 * resolving first and treat {@code ActivityNotFoundException} as "the app is missing"
 * (File Manager+'s app list: "Application not found" for every app). Gesher answers the
 * screens it has a counterpart for:
 * <ul>
 * <li>A screen about one installed app ({@code package:} data, or the app package extra):
 *     Gesher's Control Center, on that app's details (launch, uninstall).
 * <li>A device screen Windows has: the matching {@code ms-settings:} page.
 * <li>The app list, accounts and sync, Settings' main page: the Control Center.
 * </ul>
 * Anything else stays unresolved, as before. Extras and data are never passed to Windows:
 * the URIs are fixed, and the package goes to the Control Center only when it is installed.
 */
final class SettingsIntents {
    private SettingsIntents() {
    }

    /** What an intent opens: a fixed ms-settings URI, or the Control Center (on a package). */
    static final class Target {
        final String windowsUri;
        final boolean controlCenter;
        final String packageName;

        private Target(String windowsUri, boolean controlCenter, String packageName) {
            this.windowsUri = windowsUri;
            this.controlCenter = controlCenter;
            this.packageName = packageName;
        }

        static Target windows(String uri) {
            return new Target(uri, false, null);
        }

        static Target controlCenter(String packageName) {
            return new Target(null, true, packageName);
        }

        @Override
        public String toString() {
            return controlCenter ? "control-center:" + (packageName == null ? "" : packageName)
                    : windowsUri;
        }
    }

    /** Screens about one app: they name it as package:<name> (or the APP_PACKAGE extra). */
    private static final String[] APP_SCREENS = {
            "android.settings.APPLICATION_DETAILS_SETTINGS",
            "android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION",
            "android.settings.MANAGE_UNKNOWN_APP_SOURCES",
            "android.settings.REQUEST_IGNORE_BATTERY_OPTIMIZATIONS",
            "android.settings.action.MANAGE_OVERLAY_PERMISSION",
            "android.settings.action.MANAGE_WRITE_SETTINGS",
            "android.settings.REQUEST_SCHEDULE_EXACT_ALARM",
            "android.settings.APP_LOCALE_SETTINGS",
            "android.settings.APP_OPEN_BY_DEFAULT_SETTINGS",
            "android.settings.MANAGE_APP_USE_FULL_SCREEN_INTENT",
            "android.settings.REQUEST_MANAGE_MEDIA",
            "android.settings.MANAGE_APP_LONG_RUNNING_JOBS",
            "android.settings.APP_USAGE_SETTINGS",
            "android.settings.PICTURE_IN_PICTURE_SETTINGS",
    };

    /** Screens with no single app: the Control Center's app list and accounts. */
    private static final String[] CONTROL_CENTER_SCREENS = {
            "android.settings.SETTINGS",
            "android.settings.APPLICATION_SETTINGS",
            "android.settings.MANAGE_APPLICATIONS_SETTINGS",
            "android.settings.MANAGE_ALL_APPLICATIONS_SETTINGS",
            "android.settings.MANAGE_DEFAULT_APPS_SETTINGS",
            "android.settings.APPLICATION_DEVELOPMENT_SETTINGS",
            "android.settings.SYNC_SETTINGS",
            "android.settings.ADD_ACCOUNT_SETTINGS",
            "android.settings.MANAGE_ALL_FILES_ACCESS_PERMISSION",
            "android.settings.MANAGE_UNKNOWN_APP_SOURCES",
            "android.settings.IGNORE_BATTERY_OPTIMIZATION_SETTINGS",
            "android.settings.action.MANAGE_OVERLAY_PERMISSION",
            "android.settings.action.MANAGE_WRITE_SETTINGS",
            "android.settings.USAGE_ACCESS_SETTINGS",
            "android.settings.APP_LOCALE_SETTINGS",
    };

    /** Device screens Windows Settings has a page for. */
    private static final Map<String, String> WINDOWS = new HashMap<>();

    static {
        String[][] pages = {
                {"android.settings.WIFI_SETTINGS", "ms-settings:network-wifi"},
                {"android.settings.WIFI_IP_SETTINGS", "ms-settings:network-wifi"},
                {"android.settings.panel.action.WIFI", "ms-settings:network-wifi"},
                {"android.settings.WIRELESS_SETTINGS", "ms-settings:network-status"},
                {"android.settings.DATA_USAGE_SETTINGS", "ms-settings:datausage"},
                {"android.settings.panel.action.INTERNET_CONNECTIVITY", "ms-settings:network-status"},
                {"android.settings.AIRPLANE_MODE_SETTINGS", "ms-settings:network-airplanemode"},
                {"android.settings.VPN_SETTINGS", "ms-settings:network-vpn"},
                {"android.settings.BLUETOOTH_SETTINGS", "ms-settings:bluetooth"},
                {"android.settings.LOCATION_SOURCE_SETTINGS", "ms-settings:privacy-location"},
                {"android.settings.DATE_SETTINGS", "ms-settings:dateandtime"},
                {"android.settings.LOCALE_SETTINGS", "ms-settings:regionlanguage"},
                {"android.settings.DISPLAY_SETTINGS", "ms-settings:display"},
                {"android.settings.NIGHT_DISPLAY_SETTINGS", "ms-settings:nightlight"},
                {"android.settings.SOUND_SETTINGS", "ms-settings:sound"},
                {"android.settings.panel.action.VOLUME", "ms-settings:sound"},
                {"android.settings.INPUT_METHOD_SETTINGS", "ms-settings:typing"},
                {"android.settings.HARD_KEYBOARD_SETTINGS", "ms-settings:typing"},
                {"android.settings.USER_DICTIONARY_SETTINGS", "ms-settings:typing"},
                {"android.settings.ACCESSIBILITY_SETTINGS", "ms-settings:easeofaccess"},
                {"android.settings.CAPTIONING_SETTINGS", "ms-settings:easeofaccess-closedcaptioning"},
                {"android.settings.INTERNAL_STORAGE_SETTINGS", "ms-settings:storagesense"},
                {"android.settings.MEMORY_CARD_SETTINGS", "ms-settings:storagesense"},
                {"android.settings.BATTERY_SAVER_SETTINGS", "ms-settings:batterysaver"},
                {"android.intent.action.POWER_USAGE_SUMMARY", "ms-settings:batterysaver-usagedetails"},
                {"android.settings.DEVICE_INFO_SETTINGS", "ms-settings:about"},
                {"android.settings.PRIVACY_SETTINGS", "ms-settings:privacy"},
                {"android.settings.SECURITY_SETTINGS", "ms-settings:signinoptions"},
                {"android.settings.BIOMETRIC_ENROLL", "ms-settings:signinoptions"},
                {"android.settings.FINGERPRINT_ENROLL", "ms-settings:signinoptions"},
                {"android.settings.ZEN_MODE_SETTINGS", "ms-settings:quiethours"},
                {"android.settings.ZEN_MODE_PRIORITY_SETTINGS", "ms-settings:quiethours"},
                {"android.settings.NOTIFICATION_SETTINGS", "ms-settings:notifications"},
                {"android.settings.ALL_APPS_NOTIFICATION_SETTINGS", "ms-settings:notifications"},
                {"android.settings.NOTIFICATION_POLICY_ACCESS_SETTINGS", "ms-settings:quiethours"},
                {"android.settings.CAST_SETTINGS", "ms-settings:project"},
                {"android.settings.PRINT_SETTINGS", "ms-settings:printers"},
                {"android.settings.SHOW_REGULATORY_INFO", "ms-settings:about"},
                {"android.settings.SYSTEM_UPDATE_SETTINGS", "ms-settings:windowsupdate"},
        };
        for (String[] page : pages) WINDOWS.put(page[0], page[1]);
    }

    private static boolean contains(String[] actions, String action) {
        for (String a : actions) {
            if (a.equals(action)) return true;
        }
        return false;
    }

    /** Whether the intent names Settings implicitly: no component, no other package. */
    static boolean addressesSettings(Intent intent) {
        return intent != null && intent.getComponent() == null
                && (intent.getPackage() == null
                    || "com.android.settings".equals(intent.getPackage()));
    }

    /** The package a per-app screen is about: package:<name> data, else the APP_PACKAGE extra. */
    static String packageOf(Intent intent) {
        Uri data = intent.getData();
        if (data != null && "package".equalsIgnoreCase(data.getScheme())) {
            String name = data.getSchemeSpecificPart();
            return name != null && !name.isEmpty() ? name : null;
        }
        try {
            String extra = intent.getStringExtra("android.provider.extra.APP_PACKAGE");
            return extra != null && !extra.isEmpty() ? extra : null;
        } catch (RuntimeException e) {
            return null;  // an extra of another type, or one that does not unparcel
        }
    }

    /** A package name as PackageParser accepts one, so nothing else reaches the Control Center. */
    static boolean validPackageName(String name) {
        if (name == null || name.isEmpty() || name.length() > 255) return false;
        boolean segmentStart = true;
        int segments = 1;
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            if (c == '.') {
                if (segmentStart) return false;
                segmentStart = true;
                segments++;
                continue;
            }
            boolean letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            boolean other = (c >= '0' && c <= '9') || c == '_';
            if (segmentStart ? !letter : !(letter || other)) return false;
            segmentStart = false;
        }
        return !segmentStart && segments >= 2;
    }

    /**
     * What a Settings intent opens, or null when Gesher has no counterpart.
     *
     * @param installed whether a package is installed (the per-app screens need one)
     */
    static Target resolve(Intent intent, java.util.function.Predicate<String> installed) {
        if (!addressesSettings(intent)) return null;
        String action = intent.getAction();
        if (action == null) return null;
        if (contains(APP_SCREENS, action)) {
            String pkg = packageOf(intent);
            if (pkg != null && validPackageName(pkg) && installed.test(pkg)) {
                return Target.controlCenter(pkg);
            }
            // An app screen for a package Gesher does not have: Settings would show its
            // "not found" (or, without data, the list) - the Control Center's list.
            if (pkg == null && contains(CONTROL_CENTER_SCREENS, action)) {
                return Target.controlCenter(null);
            }
            return null;
        }
        if (contains(CONTROL_CENTER_SCREENS, action)) return Target.controlCenter(null);
        String uri = WINDOWS.get(action);
        return uri != null ? Target.windows(uri) : null;
    }
}
