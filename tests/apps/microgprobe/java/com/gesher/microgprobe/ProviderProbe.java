/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.microgprobe;

import android.app.Activity;
import android.app.ContentProviderHolder;
import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.content.AttributionSource;
import android.content.IContentProvider;
import android.content.IContentService;
import android.content.ContentResolver;
import android.database.IContentObserver;
import android.net.Uri;
import android.content.pm.ApplicationInfo;
import android.content.pm.ProviderInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.ServiceManager;
import android.provider.Settings;
import android.util.Log;

import java.lang.reflect.Proxy;
import java.util.List;

/** Negative content-provider probes run from an actual attached unprivileged app. */
final class ProviderProbe {
    private interface Operation { void run() throws Exception; }
    private static void denied(String name, Operation operation) throws Exception {
        try { operation.run(); }
        catch (SecurityException expected) { Log.i("microgprobe", "PASS " + name); return; }
        throw new AssertionError("Unauthorized provider operation accepted: " + name);
    }

    static void run(Activity source, IActivityManager activity, IApplicationThread thread) throws Exception {
        denied("provider claimed package spoof", () -> activity.getContentProvider(thread,
                "com.google.android.gms", "com.google.android.gms.auth.accounts", 0, true));
        denied("host-only external provider access", () -> activity.getContentProviderExternal(
                "com.google.android.gms.auth.accounts", 0, new Binder(), "probe"));
        ProviderInfo forged = new ProviderInfo();
        forged.name = "org.microg.gms.auth.AccountContentProvider";
        forged.packageName = "com.google.android.gms";
        forged.authority = "com.google.android.gms.auth.accounts";
        forged.applicationInfo = new ApplicationInfo();
        forged.applicationInfo.packageName = forged.packageName;
        forged.applicationInfo.uid = android.os.Process.myUid();
        ContentProviderHolder holder = new ContentProviderHolder(forged);
        Binder binder = new Binder("android.content.IContentProvider");
        holder.provider = (IContentProvider) Proxy.newProxyInstance(
                IContentProvider.class.getClassLoader(), new Class<?>[] {IContentProvider.class},
                (p, m, a) -> m.getName().equals("asBinder") ? binder : null);
        denied("foreign authority publication", () -> activity.publishContentProviders(thread, List.of(holder)));

        String key = "gesher_unprivileged_provider_probe";
        denied("secure settings mutation", () -> Settings.Secure.putString(source.getContentResolver(), key, "denied"));
        denied("global settings mutation", () -> Settings.Global.putString(source.getContentResolver(), key, "denied"));
        denied("system settings mutation", () -> Settings.System.putString(source.getContentResolver(), key, "denied"));
        if (Settings.Secure.getString(source.getContentResolver(), key) != null
                || Settings.Global.getString(source.getContentResolver(), key) != null
                || Settings.System.getString(source.getContentResolver(), key) != null) {
            throw new AssertionError("Denied settings write changed a table");
        }
        ContentProviderHolder settings = activity.getContentProvider(thread, source.getPackageName(), "settings", 0, true);
        Bundle extras = new Bundle(); extras.putString(Settings.NameValueTable.VALUE, "denied");
        AttributionSource impersonated = new AttributionSource.Builder(android.os.Process.myUid())
                .setPackageName("com.google.android.gms").build();
        denied("settings attribution package spoof", () -> settings.provider.call(impersonated,
                "settings", "PUT_secure", key, extras));

        IContentService content = IContentService.Stub.asInterface(ServiceManager.getService("content"));
        if (content == null) throw new AssertionError("Missing content service");
        Uri privateUri = Uri.parse("content://com.gesher.devsvc.notes/private-observer-probe");
        IContentObserver observer = new IContentObserver.Stub() {
            @Override public void onChange(boolean self, Uri uri, int user) { }
            @Override public void onChangeEtc(boolean self, Uri[] uris, int flags, int user) { }
        };
        denied("private content observer read", () -> content.registerContentObserver(privateUri, true, observer, 0, 36));
        denied("forged content notifier package", () -> content.notifyChange(new Uri[]{privateUri}, null,
                false, ContentResolver.NOTIFY_SYNC_TO_NETWORK, 0, 36, "com.gesher.devsvc"));
        denied("private content notify without write", () -> content.notifyChange(new Uri[]{privateUri}, null,
                false, ContentResolver.NOTIFY_SYNC_TO_NETWORK, 0, 36, source.getPackageName()));
        denied("master sync mutation", () -> content.setMasterSyncAutomatically(false));
        denied("cross-user content observer", () -> content.registerContentObserver(privateUri, true, observer, 10, 36));
    }
}
