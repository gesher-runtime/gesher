/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.Manifest;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.content.res.XmlResourceParser;
import android.os.Parcel;
import android.service.textservice.SpellCheckerService;
import android.util.DisplayMetrics;
import android.view.textservice.SpellCheckerInfo;
import android.view.textservice.SpellCheckerSubtype;

import com.android.internal.R;

import org.xmlpull.v1.XmlPullParser;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/** The enabled APK services declaring Android's spell-checker contract. */
final class SpellCheckerCatalog {
    private static final String METADATA = "android.view.textservice.scs";
    private static final int FLAGS = PackageManager.GET_META_DATA;
    private final PackageManagerService mPackages;
    private final Map<ComponentName, Entry> mCache = new HashMap<>();

    SpellCheckerCatalog(PackageManagerService packages) { mPackages = packages; }

    static final class Entry {
        final ApkPackage owner;
        final SpellCheckerInfo info;
        final List<SpellCheckerSubtype> subtypes;
        private String identity;
        private boolean identityRead;

        Entry(ApkPackage owner, SpellCheckerInfo info) {
            this.owner = owner;
            this.info = info;
            List<SpellCheckerSubtype> items = new ArrayList<>();
            if (info != null) {
                for (int i = 0; i < info.getSubtypeCount(); i++) items.add(info.getSubtypeAt(i));
            }
            subtypes = List.copyOf(items);
        }

        synchronized String codeIdentity() {
            if (!identityRead) {
                identity = owner.codeIdentity();
                identityRead = true;
            }
            return identity;
        }
    }

    synchronized List<Entry> available() {
        List<ResolveInfo> services;
        Map<String, ApkPackage> packages = new HashMap<>();
        synchronized (mPackages) {
            services = mPackages.services(new Intent(SpellCheckerService.SERVICE_INTERFACE),
                    null, FLAGS);
            for (ResolveInfo resolve : services) {
                if (resolve.serviceInfo == null) continue;
                String name = resolve.serviceInfo.packageName;
                packages.put(name, mPackages.get(name));
            }
        }
        mCache.entrySet().removeIf(e -> packages.get(e.getKey().getPackageName())
                != e.getValue().owner);
        List<Entry> result = new ArrayList<>();
        for (ResolveInfo resolve : services) {
            ServiceInfo service = resolve.serviceInfo;
            if (service == null || service.metaData == null
                    || !Manifest.permission.BIND_TEXT_SERVICE.equals(service.permission)) continue;
            ApkPackage owner = packages.get(service.packageName);
            if (owner == null) continue;
            ComponentName name = new ComponentName(service.packageName, service.name);
            Entry entry = mCache.computeIfAbsent(name, ignored -> new Entry(owner, parse(resolve)));
            if (entry.info != null && current(entry)) result.add(entry);
        }
        return result;
    }

    boolean ownerCurrent(Entry entry) {
        String name = entry.owner.packageName;
        return mPackages.get(name) == entry.owner && !mPackages.isRemovalPending(name);
    }

    boolean current(Entry entry) {
        return ownerCurrent(entry) && entry.owner.componentEnabled(
                entry.info.getComponent().getClassName(), ApkPackage.ComponentKind.SERVICE);
    }

    static SpellCheckerSubtype subtype(List<SpellCheckerSubtype> subtypes, int selected,
            boolean implicit, Locale deviceLocale) {
        if (selected == 0 && !implicit) return null;
        Locale wanted = deviceLocale == null ? null : deviceLocale.stripExtensions();
        SpellCheckerSubtype sameLanguage = null;
        for (SpellCheckerSubtype subtype : subtypes) {
            if (selected != 0) {
                if (subtype.hashCode() == selected) return subtype;
                continue;
            }
            Locale candidate = subtype.getLocaleObject();
            if (candidate == null || wanted == null) continue;
            if (candidate.stripExtensions().equals(wanted)) return subtype;
            if (sameLanguage == null && !candidate.getLanguage().isEmpty()
                    && candidate.getLanguage().equals(wanted.getLanguage())) sameLanguage = subtype;
        }
        return sameLanguage;
    }

    private static SpellCheckerInfo parse(ResolveInfo resolve) {
        ServiceInfo service = resolve.serviceInfo;
        int resource = service.metaData.getInt(METADATA);
        if (resource == 0) return null;
        ApplicationInfo app = service.applicationInfo;
        try (AssetManager assets = new AssetManager()) {
            if (assets.addAssetPath(app.sourceDir) == 0) return null;
            if (app.splitSourceDirs != null) {
                for (String split : app.splitSourceDirs) {
                    if (assets.addAssetPath(split) == 0) return null;
                }
            }
            DisplayMetrics metrics = new DisplayMetrics();
            metrics.setToDefaults();
            Configuration config = new Configuration();
            config.setToDefaults();
            Resources res = new Resources(assets, metrics, config);
            try (XmlResourceParser xml = res.getXml(resource)) {
                int event;
                do { event = xml.next(); }
                while (event != XmlPullParser.END_DOCUMENT && event != XmlPullParser.START_TAG);
                if (event != XmlPullParser.START_TAG || !"spell-checker".equals(xml.getName())) return null;
                int label;
                String settingsActivity;
                TypedArray attributes = res.obtainAttributes(xml, R.styleable.SpellChecker);
                try {
                    label = attributes.getResourceId(R.styleable.SpellChecker_label, 0);
                    settingsActivity = attributes.getString(R.styleable.SpellChecker_settingsActivity);
                } finally {
                    attributes.recycle();
                }
                int depth = xml.getDepth();
                List<SpellCheckerSubtype> subtypes = new ArrayList<>();
                while ((event = xml.next()) != XmlPullParser.END_DOCUMENT) {
                    if (event == XmlPullParser.END_TAG && xml.getDepth() == depth) break;
                    if (event != XmlPullParser.START_TAG) continue;
                    if (xml.getDepth() != depth + 1 || !"subtype".equals(xml.getName())) return null;
                    subtypes.add(parseSubtype(res, xml));
                }
                if (subtypes.isEmpty()) return null;
                return checkerInfo(resolve, label, settingsActivity, subtypes);
            }
        } catch (Exception failure) {
            SystemServer.log("textservices: invalid metadata for " + service.packageName + "/"
                    + service.name + " (" + failure.getClass().getSimpleName() + ")");
            return null;
        }
    }

    private static SpellCheckerSubtype parseSubtype(Resources res, XmlResourceParser xml) {
        TypedArray a = res.obtainAttributes(xml, R.styleable.SpellChecker_Subtype);
        try {
            return new SpellCheckerSubtype(a.getResourceId(R.styleable.SpellChecker_Subtype_label, 0),
                    a.getString(R.styleable.SpellChecker_Subtype_subtypeLocale),
                    a.getString(R.styleable.SpellChecker_Subtype_languageTag),
                    a.getString(R.styleable.SpellChecker_Subtype_subtypeExtraValue),
                    a.getInt(R.styleable.SpellChecker_Subtype_subtypeId, 0));
        } finally {
            a.recycle();
        }
    }

    /** The API 36 Parcelable avoids constructing an application Context in system_server. */
    private static SpellCheckerInfo checkerInfo(ResolveInfo resolve, int label, String settingsActivity,
            List<SpellCheckerSubtype> subtypes) {
        ServiceInfo service = resolve.serviceInfo;
        Parcel parcel = Parcel.obtain();
        try {
            parcel.writeInt(label);
            parcel.writeString(new ComponentName(service.packageName, service.name).flattenToShortString());
            parcel.writeString(settingsActivity);
            resolve.writeToParcel(parcel, 0);
            parcel.writeTypedList(subtypes);
            parcel.setDataPosition(0);
            return SpellCheckerInfo.CREATOR.createFromParcel(parcel);
        } finally {
            parcel.recycle();
        }
    }
}
