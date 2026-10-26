/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ILocaleManager;
import android.app.LocaleConfig;
import android.content.Intent;
import android.content.res.Configuration;
import android.os.Binder;
import android.os.LocaleList;
import android.provider.Settings;

import com.gesher.app.DeviceProfile;
import com.gesher.app.locale.SystemLocale;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Properties;

/**
 * {@code locale}: LocaleManagerService, and the configuration every app gets (locale track,
 * docs/locale.md, ADR 0049).
 *
 * <ul>
 * <li><b>Per-app locales.</b> {@code LocaleManager.setApplicationLocales} (and
 *     AppCompatDelegate.setApplicationLocales, which calls it on API 33+) keeps the app's
 *     locales per package in {@code $ANDROID_DATA/system/app_locales} (ATMS's
 *     PackageConfigPersister keeps them in /data/system); {@link #configurationFor} puts
 *     them in front of the system's in that package's Configuration, as
 *     LocaleOverlayHelper.combineLocalesIfOverlayExists does, with the layout direction of
 *     the first. A change is a configuration change of the package's running process and
 *     activities ({@link ActivityTaskManagerService#dispatchConfiguration}: relaunched, or
 *     onConfigurationChanged when they handle {@code locale}) and an ACTION_LOCALE_CHANGED
 *     broadcast to the package, as on Android 13+.
 * <li><b>Windows' settings changing.</b> The system server's hidden events window
 *     ({@link WindowsHost}) gets WM_SETTINGCHANGE; the languages, regional format and time
 *     format are read again ({@link SystemLocale#refresh}) and, when they changed, every
 *     running app gets the new configuration, ACTION_LOCALE_CHANGED (every package's
 *     registered and manifest receivers, an exempt broadcast) and ACTION_CONFIGURATION_CHANGED
 *     (registered receivers); a changed 12/24-hour format is Settings.System.TIME_12_24 and
 *     ACTION_TIME_CHANGED with EXTRA_TIME_PREF_24_HOUR_FORMAT, as the Settings app sends it.
 * <li><b>Configurations</b> carry a sequence number ({@code Configuration.seq}) that grows
 *     with each change, so an app process applies them in order (ConfigurationController
 *     drops older ones).
 * </ul>
 *
 * <p>In an app's own process (gesher-app's single-process mode, no activity manager drives
 * the activity) a change is applied to the process directly ({@link #applyInProcess}).
 */
public final class LocaleManagerService extends ILocaleManager.Default
        implements WindowsHost.Listener {
    private static final String TAG = "locale: ";

    private static volatile LocaleManagerService sInstance;
    /** The configuration's sequence number: grows with every change. */
    private static int sSeq = 1;  // guarded by LocaleManagerService.class

    private final ActivityManagerService mAm;
    private final ActivityTaskManagerService mAtm;
    private final PackageManagerService mPm;
    private final File mFile = new File(Device.dataDir(), "system/app_locales");
    // Guarded by this: package -> language tags; package -> LocaleConfig override.
    private final Properties mLocales;
    private final Properties mOverrides = new Properties();
    /** The package of this process, in an app's own process (single-process mode). */
    private volatile String mInProcessPackage;

    LocaleManagerService(ActivityManagerService am, ActivityTaskManagerService atm,
            PackageManagerService pm) {
        mAm = am;
        mAtm = atm;
        mPm = pm;
        Properties p = Device.read(mFile);
        mLocales = p != null ? p : new Properties();
        sInstance = this;
        if (WindowsHost.inSystemServer()) {
            WindowsHost.addListener(this);
            List<String> custom = SystemLocale.customFormats();
            SystemServer.log(TAG + "the device's locales " + SystemLocale.describe()
                    + (custom.isEmpty() ? "" : "; Windows' custom formats not carried over"
                            + " (Android has no setting for them): " + String.join(", ", custom)));
        } else if (pm.packages().size() == 1) {
            // gesher-app in a single process: its package, and Windows' changes applied here.
            mInProcessPackage = pm.packages().iterator().next().packageName;
            WindowsHost.addListener(this);
        }
    }

    // --- the configuration -------------------------------------------------------------

    /**
     * The configuration of {@code packageName}'s processes and activities: the device's
     * ({@link DeviceProfile#configuration}) with the app's own locales in front, and the
     * current sequence number. Null package: the device's.
     */
    public static Configuration configurationFor(String packageName) {
        Configuration c = DeviceProfile.configuration();
        LocaleManagerService s = sInstance;
        LocaleList app = s != null && packageName != null ? s.appLocales(packageName) : null;
        if (app != null && !app.isEmpty()) {
            LocaleList combined = combine(app, c.getLocales());
            c.setLocales(combined);
            c.setLayoutDirection(combined.get(0));
        }
        synchronized (LocaleManagerService.class) {
            // Read geometry under the same sequence lock: an older rotation
            // snapshot must not receive a newer concurrent change's sequence.
            com.gesher.app.window.ApplicationDisplay.applyConfiguration(c);
            c.seq = sSeq;
        }
        return c;
    }

    /** The shared Application display changed while task overrides stay local. */
    public static void onSharedDisplayChanged() {
        LocaleManagerService service = sInstance;
        if (service == null) return;
        nextSeq();
        if (service.mInProcessPackage != null) {
            applyInProcess(configurationFor(service.mInProcessPackage), "shared display rotation");
        } else {
            service.mAtm.dispatchConfiguration(null);
        }
    }

    /**
     * The configuration of the process calling in (a window session's relayout answers
     * it): its package's, else this process's package's in an app's own process.
     */
    static Configuration configurationForCaller() {
        int pid = Binder.getCallingPid();
        LocaleManagerService s = sInstance;
        String pkg = null;
        if (s != null) {
            ActivityTaskManagerService.ProcessRecord app = s.mAtm.callerProcess(pid);
            pkg = app != null ? app.packageName : s.mInProcessPackage;
        }
        return configurationFor(pkg);
    }

    /** LocaleOverlayHelper.combineLocales: the app's, then the system's not among them. */
    private static LocaleList combine(LocaleList app, LocaleList system) {
        List<Locale> all = new ArrayList<>();
        for (int i = 0; i < app.size(); i++) all.add(app.get(i));
        for (int i = 0; i < system.size(); i++) {
            if (!all.contains(system.get(i))) all.add(system.get(i));
        }
        return new LocaleList(all.toArray(new Locale[0]));
    }

    private static void nextSeq() {
        synchronized (LocaleManagerService.class) {
            sSeq++;
        }
    }

    private synchronized LocaleList appLocales(String packageName) {
        String tags = mLocales.getProperty(packageName);
        return tags == null || tags.isEmpty() ? null : LocaleList.forLanguageTags(tags);
    }

    // --- ILocaleManager ----------------------------------------------------------------

    @Override
    public void setApplicationLocales(String packageName, int userId, LocaleList locales,
            boolean fromDelegate) {
        if (packageName == null || locales == null) {
            throw new NullPointerException("packageName and locales must not be null");
        }
        if (mPm.get(packageName) == null) {
            throw new IllegalArgumentException("Unknown package name " + packageName);
        }
        String tags = locales.toLanguageTags();
        boolean changed;
        synchronized (this) {
            String old = mLocales.getProperty(packageName, "");
            changed = !old.equals(tags);
            if (tags.isEmpty()) mLocales.remove(packageName);
            else mLocales.setProperty(packageName, tags);
            if (changed) persist();
        }
        SystemServer.log(TAG + packageName + "'s app locales "
                + (tags.isEmpty() ? "cleared (the system's)" : "set to " + tags)
                + (fromDelegate ? " (by a delegate)" : "") + (changed ? "" : ", unchanged"));
        if (!changed) return;
        nextSeq();
        if (mInProcessPackage != null) {
            applyInProcess(configurationFor(mInProcessPackage), "app locales " + tags);
            return;
        }
        mAtm.dispatchConfiguration(packageName);
        // ACTION_LOCALE_CHANGED to the app whose locales changed (Android 13+: its effective
        // locale changed); ACTION_APPLICATION_LOCALE_CHANGED goes to an installer, of which
        // the device has none.
        Intent i = new Intent(Intent.ACTION_LOCALE_CHANGED);
        i.addFlags(Intent.FLAG_RECEIVER_FOREGROUND | Intent.FLAG_RECEIVER_INCLUDE_BACKGROUND);
        i.setPackage(packageName);
        broadcast(i);
    }

    @Override
    public LocaleList getApplicationLocales(String packageName, int userId) {
        LocaleList l = packageName != null ? appLocales(packageName) : null;
        return l != null ? l : LocaleList.getEmptyLocaleList();
    }

    @Override
    public LocaleList getSystemLocales() {
        return SystemLocale.locales();
    }

    @Override
    public void setOverrideLocaleConfig(String packageName, int userId,
            LocaleConfig localeConfig) {
        synchronized (this) {
            LocaleList l = localeConfig != null ? localeConfig.getSupportedLocales() : null;
            if (l == null) mOverrides.remove(packageName);
            else mOverrides.setProperty(packageName, l.toLanguageTags());
        }
        SystemServer.log(TAG + packageName + "'s LocaleConfig override "
                + (localeConfig != null ? localeConfig.getSupportedLocales() : "removed"));
    }

    @Override
    public LocaleConfig getOverrideLocaleConfig(String packageName, int userId) {
        String tags;
        synchronized (this) {
            tags = mOverrides.getProperty(packageName);
        }
        return tags == null ? null : new LocaleConfig(LocaleList.forLanguageTags(tags));
    }

    private void persist() {
        try {
            Device.write(mFile, mLocales);
        } catch (IOException e) {
            SystemServer.log(TAG + "the app locales were not saved: " + e);
        }
    }

    // --- Windows' settings --------------------------------------------------------------

    @Override
    public void onWindowsEvents(int events) {
        if ((events & WindowsHost.EVENT_SETTING_CHANGED) == 0) return;
        boolean old24 = SystemLocale.is24Hour();
        String change = SystemLocale.refresh();
        if (change == null) return;
        nextSeq();
        LocaleList.setDefault(SystemLocale.locales());
        boolean now24 = SystemLocale.is24Hour();
        SystemServer.log(TAG + "Windows' settings changed: " + change + "; the device's locales "
                + SystemLocale.describe());
        if (old24 != now24) SettingsProvider.putSystem(Settings.System.TIME_12_24, now24 ? "24" : "12");
        if (mInProcessPackage != null) {
            applyInProcess(configurationFor(mInProcessPackage), change);
            return;
        }
        mAtm.dispatchConfiguration(null);
        Intent locale = new Intent(Intent.ACTION_LOCALE_CHANGED);
        locale.addFlags(Intent.FLAG_RECEIVER_FOREGROUND | Intent.FLAG_RECEIVER_INCLUDE_BACKGROUND);
        broadcastToAll(locale);
        Intent config = new Intent(Intent.ACTION_CONFIGURATION_CHANGED);
        config.addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY | Intent.FLAG_RECEIVER_FOREGROUND);
        broadcast(config);
        if (old24 != now24) {
            Intent time = new Intent(Intent.ACTION_TIME_CHANGED);
            time.putExtra(Intent.EXTRA_TIME_PREF_24_HOUR_FORMAT, now24
                    ? Intent.EXTRA_TIME_PREF_VALUE_USE_24_HOUR
                    : Intent.EXTRA_TIME_PREF_VALUE_USE_12_HOUR);
            broadcastToAll(time);
        }
    }

    /**
     * An exempt implicit broadcast: every package's manifest receivers (a broadcast to each
     * package, BroadcastController's rule for manifest receivers) and registered ones.
     */
    private void broadcastToAll(Intent intent) {
        for (ApkPackage p : new ArrayList<>(mPm.packages())) {
            Intent i = new Intent(intent);
            i.setPackage(p.packageName);
            broadcast(i);
        }
    }

    private void broadcast(Intent intent) {
        try {
            mAm.sendBroadcastFor("android", intent, null);
        } catch (Throwable t) {
            SystemServer.log(TAG + intent.getAction() + " failed: " + t);
        }
    }

    // --- an app's own process -----------------------------------------------------------

    /**
     * A configuration change in this process (single-process mode: no activity manager
     * drives its activity): ActivityThread.handleConfigurationChanged on the main thread (the
     * resources, the default locales, Application and other non-UI callbacks'
     * onConfigurationChanged), then the activity's onConfigurationChanged when it handles
     * every change; otherwise it is not recreated (only the Gesher device relaunches
     * activities) and that is logged.
     */
    private static void applyInProcess(Configuration config, String why) {
        android.os.Handler main = new android.os.Handler(android.os.Looper.getMainLooper());
        main.post(() -> {
            android.app.ActivityThread thread = android.app.ActivityThread.currentActivityThread();
            if (thread == null) return;
            android.app.Activity activity = android.app.GesherAppDriver.currentActivity();
            Configuration before = activity != null
                    ? new Configuration(activity.getResources().getConfiguration()) : null;
            try {
                thread.handleConfigurationChanged(config, android.content.Context.DEVICE_ID_DEFAULT);
            } catch (Throwable t) {
                SystemServer.log(TAG + "the configuration change (" + why + ") failed in this"
                        + " process: " + t);
                return;
            }
            SystemServer.log(TAG + "configuration change in this process (" + why + "): "
                    + config.getLocales().toLanguageTags());
            if (activity == null || before == null) return;
            Configuration after = new Configuration(activity.getResources().getConfiguration());
            int diff = before.diffPublicOnly(after);
            int handled = activity.getApplicationInfo() != null
                    ? activityConfigChanges(activity) : 0;
            if (diff != 0 && (diff & ~handled) == 0) {
                activity.onConfigurationChanged(after);
                SystemServer.log(TAG + activity.getComponentName().getShortClassName()
                        + " onConfigurationChanged (it handles the change)");
            } else if (diff != 0) {
                SystemServer.log(TAG + activity.getComponentName().getShortClassName()
                        + " is not recreated in a single process (changes 0x"
                        + Integer.toHexString(diff) + "; only the Gesher device relaunches"
                        + " activities)");
            }
        });
    }

    private static int activityConfigChanges(android.app.Activity activity) {
        try {
            return activity.getPackageManager().getActivityInfo(activity.getComponentName(), 0)
                    .getRealConfigChanged();
        } catch (Exception e) {
            return 0;
        }
    }
}
