/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.os.Binder;
import android.os.Bundle;
import android.view.textservice.SpellCheckerInfo;
import android.view.textservice.SpellCheckerSubtype;

import com.android.internal.textservice.ISpellCheckerSessionListener;
import com.android.internal.textservice.ITextServicesManager;
import com.android.internal.textservice.ITextServicesSessionListener;
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

    TextServicesManagerService(ActivityManagerService activity, PackageManagerService packages) {
        mActivity = activity;
        mPackages = packages;
        mCatalog = new SpellCheckerCatalog(packages);
        mSessions = new SpellCheckerSessions(activity, mCatalog);
        packages.onPackageRemoval(mSessions::packageRemoving);
    }

    private static void user(int userId) {
        if (userId != 0) throw new SecurityException("Only text-services user 0 is supported");
    }

    private static Locale deviceLocale() { return SystemLocale.locales().get(0); }

    private SettingsProvider.SpellCheckerSettings settings() {
        return mActivity.settingsProvider().spellCheckerSettings();
    }

    private Entry selected(List<Entry> entries, SettingsProvider.SpellCheckerSettings settings) {
        if (entries.isEmpty()) return null;
        Entry localeMatch = null;
        Entry languageMatch = null;
        Entry firstSystem = null;
        Locale locale = deviceLocale().stripExtensions();
        for (Entry entry : entries) {
            if (entry.info.getId().equals(settings.component)
                    && settings.identity != null && settings.identity.equals(entry.codeIdentity())) {
                return entry;
            }
            // Only trusted, preinstalled providers may receive text automatically.
            if (!entry.owner.isSystemAddon()) continue;
            if (firstSystem == null) firstSystem = entry;
            SpellCheckerSubtype match = SpellCheckerCatalog.subtype(entry.subtypes, 0, true, locale);
            if (match == null) continue;
            if (localeMatch == null && match.getLocaleObject().stripExtensions().equals(locale)) {
                localeMatch = entry;
            }
            if (languageMatch == null) languageMatch = entry;
        }
        if (localeMatch != null) return localeMatch;
        return languageMatch == null ? firstSystem : languageMatch;
    }

    @Override public SpellCheckerInfo getCurrentSpellChecker(int userId, String locale) {
        user(userId);
        Entry entry = selected(mCatalog.available(), settings());
        return entry == null ? null : entry.info;
    }

    @Override public SpellCheckerSubtype getCurrentSpellCheckerSubtype(int userId,
            boolean allowImplicitlySelectedSubtype) {
        user(userId);
        SettingsProvider.SpellCheckerSettings settings = settings();
        Entry entry = selected(mCatalog.available(), settings);
        return entry == null ? null : SpellCheckerCatalog.subtype(entry.subtypes, settings.subtype,
                allowImplicitlySelectedSubtype, deviceLocale());
    }

    @Override public boolean isSpellCheckerEnabled(int userId) {
        user(userId);
        SettingsProvider.SpellCheckerSettings settings = settings();
        return settings.enabled && selected(mCatalog.available(), settings) != null;
    }

    private boolean accessible(Entry entry, Entry selected, String caller) {
        return entry == selected || "android".equals(caller)
                || mPackages.canSeeInstalledPackage(caller, entry.owner.packageName);
    }

    @Override public SpellCheckerInfo[] getEnabledSpellCheckers(int userId) {
        user(userId);
        String caller = mActivity.callers().callerPackage();
        List<Entry> entries = mCatalog.available();
        Entry selected = selected(entries, settings());
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
        Entry selected = selected(entries, settings());
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
