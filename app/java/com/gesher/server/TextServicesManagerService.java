/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.os.Binder;
import android.os.Bundle;
import android.view.textservice.SpellCheckerInfo;
import android.view.textservice.SpellCheckerSubtype;

import com.android.internal.textservice.ISpellCheckerSessionListener;
import com.android.internal.textservice.ITextServicesManager;
import com.android.internal.textservice.ITextServicesSessionListener;
import com.android.internal.inputmethod.SubtypeLocaleUtils;
import com.gesher.app.locale.SystemLocale;
import com.gesher.server.SpellCheckerCatalog.Entry;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/** TextServicesManager for user 0, backed by installed Android spell-checker services. */
public final class TextServicesManagerService extends ITextServicesManager.Default {
    private final ActivityManagerService mActivity;
    private final PackageManagerService mPackages;
    private final SpellCheckerCatalog mCatalog;
    private final SpellCheckerSessions mSessions;
    // The previous package-event snapshot is needed for Android's replacement policy.
    // Getters never change the chosen provider merely because the locale changed.
    private List<Entry> mLastCatalog = List.of();

    TextServicesManagerService(ActivityManagerService activity, PackageManagerService packages) {
        mActivity = activity;
        mPackages = packages;
        mCatalog = new SpellCheckerCatalog(packages);
        mSessions = new SpellCheckerSessions(activity, mCatalog);
        activity.settingsProvider().setAutomaticCheckerSelector(this::selectAutomatic);
        packages.onPackageRemoval(mSessions::packageRemoving);
        packages.onPackageEvent(this::packageChanged);
        initializeSelection(true, null, false);
    }

    private static void user(int userId) {
        if (userId != 0) throw new SecurityException("Only text-services user 0 is supported");
    }

    private static Locale deviceLocale() { return SystemLocale.locales().get(0); }

    private SettingsProvider.SpellCheckerSettings settings() {
        return mActivity.settingsProvider().spellCheckerSettings();
    }

    private boolean selectAutomatic() {
        SettingsProvider.SpellCheckerSettings before = settings();
        Entry choice = defaultEntry(mCatalog.available(), null, deviceLocale());
        return mActivity.settingsProvider().initializeSpellChecker(before, choice);
    }

    private Entry selected(List<Entry> entries, SettingsProvider.SpellCheckerSettings settings) {
        Entry current = configured(entries, settings);
        return current != null ? current : defaultEntry(entries, null, deviceLocale());
    }

    private static Entry configured(List<Entry> entries, SettingsProvider.SpellCheckerSettings settings) {
        for (Entry entry : entries) {
            if (entry.info.getId().equals(settings.component)
                    && settings.identity != null && settings.identity.equals(entry.codeIdentity())) {
                return entry;
            }
        }
        return null;
    }

    private void packageChanged(Intent event) {
        String action = event.getAction();
        if (!Intent.ACTION_PACKAGE_ADDED.equals(action)
                && !Intent.ACTION_PACKAGE_REMOVED.equals(action)
                && !Intent.ACTION_PACKAGE_CHANGED.equals(action)) return;
        String name = event.getData() == null ? null : event.getData().getSchemeSpecificPart();
        initializeSelection(false, name, Intent.ACTION_PACKAGE_REMOVED.equals(action)
                || event.getBooleanExtra(Intent.EXTRA_REPLACING, false));
    }

    private void initializeSelection(boolean initial, String changedPackage, boolean disappearing) {
        SettingsProvider.SpellCheckerSettings settings;
        Entry choice;
        synchronized (this) {
            List<Entry> entries = mCatalog.available();
            settings = settings();
            Entry previous = configured(mLastCatalog, settings);
            mLastCatalog = entries;
            if (initial) {
                if (configured(entries, settings) != null) return;
                choice = selected(entries, settings);
            } else {
                if (!settings.enabled) return;
                if (previous == null) {
                    choice = defaultEntry(entries, null, deviceLocale());
                } else if (disappearing && previous.owner.packageName.equals(changedPackage)) {
                    choice = defaultEntry(entries, changedPackage, deviceLocale());
                    if (choice != null && choice.info.getId().equals(previous.info.getId())
                            && choice.codeIdentity() != null
                            && choice.codeIdentity().equals(settings.identity)) return;
                } else {
                    return;
                }
            }
        }
        // The settings provider checks the revision and current package instance before
        // publication. Observers run without this service's monitor held.
        mActivity.settingsProvider().initializeSpellChecker(settings, choice);
    }

    private static Entry defaultEntry(List<Entry> entries, String preferredPackage, Locale locale) {
        List<SpellCheckerInfo> system = new ArrayList<>();
        for (Entry entry : entries) {
            if (!entry.owner.isSystemAddon()) continue;
            system.add(entry.info);
        }
        SpellCheckerInfo choice = defaultChecker(system, preferredPackage, locale);
        for (Entry entry : entries) if (entry.info == choice) return entry;
        return null;
    }

    /** AOSP's automatic provider choice from the current trusted system entries. */
    static SpellCheckerInfo defaultChecker(List<SpellCheckerInfo> entries, String preferredPackage, Locale locale) {
        List<SpellCheckerInfo> system = new ArrayList<>();
        for (SpellCheckerInfo info : entries) {
            if ((info.getServiceInfo().applicationInfo.flags & ApplicationInfo.FLAG_SYSTEM) == 0) continue;
            system.add(info);
            if (info.getPackageName().equals(preferredPackage)) return info;
        }
        for (Locale suitable : suitableLocales(locale)) {
            for (SpellCheckerInfo info : system) {
                for (int i = 0; i < info.getSubtypeCount(); i++) {
                    SpellCheckerSubtype subtype = info.getSubtypeAt(i);
                    // AOSP automatic provider selection uses the legacy subtype locale;
                    // implicit subtype selection separately uses getLocaleObject().
                    if (suitable.equals(SubtypeLocaleUtils.constructLocaleFromString(subtype.getLocale()))) {
                        return info;
                    }
                }
            }
        }
        return system.isEmpty() ? null : system.get(0);
    }

    /** Android textservices LocaleUtils fallback order, including the English fallback. */
    private static List<Locale> suitableLocales(Locale locale) {
        List<Locale> result = new ArrayList<>();
        String language = locale == null ? "" : locale.getLanguage();
        String country = locale == null ? "" : locale.getCountry();
        String variant = locale == null ? "" : locale.getVariant();
        Locale languageCountry = language.isEmpty() || country.isEmpty() ? null : new Locale(language, country);
        if (languageCountry != null && !variant.isEmpty()) result.add(new Locale(language, country, variant));
        if ("en".equals(language)) {
            if (languageCountry == null) {
                result.add(Locale.ENGLISH); result.add(Locale.US); result.add(Locale.UK);
            } else {
                result.add(languageCountry);
                if (!Locale.US.equals(languageCountry)) result.add(Locale.US);
                if (!Locale.UK.equals(languageCountry)) result.add(Locale.UK);
                result.add(Locale.ENGLISH);
            }
        } else {
            if (languageCountry != null) result.add(languageCountry);
            if (!language.isEmpty()) result.add(new Locale(language));
            result.add(Locale.US); result.add(Locale.UK); result.add(Locale.ENGLISH);
        }
        return result;
    }

    @Override public SpellCheckerInfo getCurrentSpellChecker(int userId, String locale) {
        user(userId);
        Entry entry = configured(mCatalog.available(), settings());
        return entry == null ? null : entry.info;
    }

    @Override public SpellCheckerSubtype getCurrentSpellCheckerSubtype(int userId,
            boolean allowImplicitlySelectedSubtype) {
        user(userId);
        SettingsProvider.SpellCheckerSettings settings = settings();
        Entry entry = configured(mCatalog.available(), settings);
        return entry == null ? null : SpellCheckerCatalog.subtype(entry.subtypes, settings.subtype,
                allowImplicitlySelectedSubtype, deviceLocale());
    }

    @Override public boolean isSpellCheckerEnabled(int userId) {
        user(userId);
        return settings().enabled;
    }

    private boolean accessible(Entry entry, Entry selected, String caller) {
        return entry == selected || "android".equals(caller)
                || mPackages.canSeeInstalledPackage(caller, entry.owner.packageName);
    }

    @Override public SpellCheckerInfo[] getEnabledSpellCheckers(int userId) {
        user(userId);
        String caller = mActivity.callers().callerPackage();
        List<Entry> entries = mCatalog.available();
        Entry selected = configured(entries, settings());
        List<SpellCheckerInfo> visible = new ArrayList<>();
        for (Entry entry : entries) {
            if (accessible(entry, selected, caller)) visible.add(entry.info);
        }
        // Android's public getEnabledSpellCheckerInfos() normalizes null to an empty list.
        return visible.isEmpty() ? null : visible.toArray(new SpellCheckerInfo[0]);
    }

    @Override public void getSpellCheckerService(int userId, String checkerId, String locale,
            ITextServicesSessionListener sessionListener, ISpellCheckerSessionListener listener,
            Bundle extras, int supportedAttributes) {
        user(userId);
        if (checkerId == null || checkerId.isEmpty() || sessionListener == null || listener == null) return;
        String caller = mActivity.callers().callerPackage();
        List<Entry> entries = mCatalog.available();
        Entry selected = configured(entries, settings());
        for (Entry entry : entries) {
            if (checkerId.equals(entry.info.getId()) && accessible(entry, selected, caller)) {
                mSessions.request(entry, Binder.getCallingPid(), locale, sessionListener, listener,
                        extras, supportedAttributes);
                return;
            }
        }
    }

    @Override public void finishSpellCheckerService(int userId, ISpellCheckerSessionListener listener) {
        user(userId);
        if (listener == null) return;
        mActivity.callers().callerPackage();
        mSessions.finish(Binder.getCallingPid(), listener.asBinder());
    }
}
