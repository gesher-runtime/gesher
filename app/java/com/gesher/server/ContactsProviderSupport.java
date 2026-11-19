/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ContentProviderHolder;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.IContentProvider;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Build;
import android.os.Process;
import com.android.internal.pm.pkg.component.ParsedProvider;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * Shared system-owned metadata, caller permission enforcement, URI parameter
 * validation, and package admission rules for the built-in {@code CallLog}
 * and {@code Contacts} providers ({@code com.android.providers.contacts}).
 */
final class ContactsProviderSupport {
    static final String PACKAGE = "com.android.providers.contacts";
    static final String CONTACTS_AUTHORITY = "com.android.contacts";
    static final String LEGACY_CONTACTS_AUTHORITY = "contacts";
    static final String CONTACTS_AUTHORITIES = CONTACTS_AUTHORITY + ";" + LEGACY_CONTACTS_AUTHORITY;
    static final String CONTACTS_CLASS = "com.android.providers.contacts.ContactsProvider2";
    static final String CALL_LOG_AUTHORITY = "call_log";
    static final String CALL_LOG_CLASS = "com.android.providers.contacts.CallLogProvider";

    static final String READ_CONTACTS = "android.permission.READ_CONTACTS";
    static final String WRITE_CONTACTS = "android.permission.WRITE_CONTACTS";
    static final String READ_CALL_LOG = "android.permission.READ_CALL_LOG";
    static final String WRITE_CALL_LOG = "android.permission.WRITE_CALL_LOG";

    private ContactsProviderSupport() { }

    static File dataDirectory() {
        return new File(Device.dataDir(), "system/contacts");
    }

    static boolean isSystemAuthority(String authority) {
        return isContactsAuthority(authority) || CALL_LOG_AUTHORITY.equals(authority);
    }

    static boolean isContactsAuthority(String authority) {
        return CONTACTS_AUTHORITY.equals(authority) || LEGACY_CONTACTS_AUTHORITY.equals(authority);
    }

    static String readPermission(String authority) {
        if (CALL_LOG_AUTHORITY.equals(authority)) return READ_CALL_LOG;
        if (isContactsAuthority(authority)) return READ_CONTACTS;
        return null;
    }

    static String writePermission(String authority) {
        if (CALL_LOG_AUTHORITY.equals(authority)) return WRITE_CALL_LOG;
        if (isContactsAuthority(authority)) return WRITE_CONTACTS;
        return null;
    }

    static ApplicationInfo applicationInfo() {
        ApplicationInfo info = new ApplicationInfo();
        info.packageName = PACKAGE;
        info.processName = "system";
        info.uid = Process.SYSTEM_UID;
        info.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_INSTALLED
                | ApplicationInfo.FLAG_HAS_CODE;
        info.enabled = true;
        info.targetSdkVersion = Build.VERSION.SDK_INT;
        info.nonLocalizedLabel = "Contacts Storage";
        String root = System.getenv("ANDROID_ROOT");
        info.sourceDir = (root == null ? "/system" : root) + "/framework/gesher-app.jar";
        info.publicSourceDir = info.sourceDir;
        info.dataDir = dataDirectory().getPath();
        return info;
    }

    static ProviderInfo contactsProviderInfo() {
        return providerInfo(CONTACTS_CLASS, CONTACTS_AUTHORITIES, READ_CONTACTS, WRITE_CONTACTS);
    }

    static ProviderInfo callLogProviderInfo() {
        return providerInfo(CALL_LOG_CLASS, CALL_LOG_AUTHORITY, READ_CALL_LOG, WRITE_CALL_LOG);
    }

    private static ProviderInfo providerInfo(String className, String authority,
            String readPermission, String writePermission) {
        ApplicationInfo ai = applicationInfo();
        ProviderInfo info = new ProviderInfo();
        info.applicationInfo = ai;
        info.packageName = PACKAGE;
        info.name = className;
        info.processName = ai.processName;
        info.authority = authority;
        info.readPermission = readPermission;
        info.writePermission = writePermission;
        info.exported = true;
        info.enabled = true;
        info.multiprocess = false;
        info.flags = ProviderInfo.FLAG_SINGLE_USER;
        return info;
    }

    static ProviderInfo withoutPermissions(ProviderInfo info) {
        ProviderInfo attached = new ProviderInfo(info);
        attached.readPermission = null;
        attached.writePermission = null;
        return attached;
    }

    static boolean matchesProviderFlags(long flags) {
        long directBoot = flags & (PackageManager.MATCH_DIRECT_BOOT_AWARE
                | PackageManager.MATCH_DIRECT_BOOT_UNAWARE);
        return directBoot != PackageManager.MATCH_DIRECT_BOOT_AWARE;
    }

    static ProviderInfo providerForAuthority(String authority, long flags) {
        if (!matchesProviderFlags(flags)) return null;
        if (CALL_LOG_AUTHORITY.equals(authority)) return callLogProviderInfo();
        if (isContactsAuthority(authority)) return contactsProviderInfo();
        return null;
    }

    static ProviderInfo providerForComponent(String className, long flags) {
        if (!matchesProviderFlags(flags)) return null;
        if (CALL_LOG_CLASS.equals(className)) return callLogProviderInfo();
        if (CONTACTS_CLASS.equals(className)) return contactsProviderInfo();
        return null;
    }

    static PackageInfo packageInfo(long flags) {
        PackageInfo info = new PackageInfo();
        info.packageName = PACKAGE;
        info.applicationInfo = applicationInfo();
        info.setLongVersionCode(Build.VERSION.SDK_INT);
        if ((flags & PackageManager.GET_PROVIDERS) != 0 && matchesProviderFlags(flags)) {
            info.providers = new ProviderInfo[] { contactsProviderInfo(), callLogProviderInfo() };
        }
        return info;
    }

    /** Rejects any APK attempting to claim the built-in contacts package or authorities. */
    static void checkPackageAdmission(ApkPackage pkg)
            throws android.content.pm.PackageParser.PackageParserException {
        if (pkg == null || pkg.parsed == null) return;
        if (PACKAGE.equals(pkg.packageName)) {
            throw new android.content.pm.PackageParser.PackageParserException(
                    PackageManager.INSTALL_FAILED_UPDATE_INCOMPATIBLE,
                    "The built-in contacts provider is updated with the runtime");
        }
        for (ParsedProvider provider : pkg.parsed.getProviders()) {
            String authorities = provider.getAuthority();
            if (authorities == null) continue;
            for (String authority : authorities.split(";")) {
                if (isSystemAuthority(authority)) {
                    throw new android.content.pm.PackageParser.PackageParserException(
                            PackageManager.INSTALL_FAILED_CONFLICTING_PROVIDER,
                            "Provider authority already owned by " + PACKAGE + ": " + authority);
                }
            }
        }
    }

    static Context contextForProvider() {
        Context system = android.app.GesherSettingsContext.get();
        return new ContextWrapper(system) {
            @Override public String getPackageName() { return PACKAGE; }
            @Override public ApplicationInfo getApplicationInfo() { return applicationInfo(); }
        };
    }

    static ContentProviderHolder holder(ProviderInfo info, IContentProvider transport) {
        ContentProviderHolder h = new ContentProviderHolder(info);
        h.provider = transport;
        h.noReleaseNeeded = true;
        return h;
    }

    static void enforce(PackageManagerService pm, CallerIdentity callers, String permission) {
        if (pm == null || callers == null) {
            if (Binder.getCallingPid() != Process.myPid()) {
                throw new SecurityException("Provider caller identity unavailable");
            }
            return;
        }
        if (Binder.getCallingPid() == Process.myPid()) return;
        if (callers.isSystem()) return;
        String actual = callers.callerPackage();
        if (permission != null && pm.permissions().checkPermission(actual, permission)
                != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("Permission Denial: requires " + permission);
        }
    }

    /** Notifies observers outside provider locks without leaking the caller's Binder identity. */
    static void notifyChange(Context context, Uri uri) {
        if (context == null || uri == null) return;
        long identity = Binder.clearCallingIdentity();
        try {
            context.getContentResolver().notifyChange(uri, null);
        } catch (RuntimeException failure) {
            SystemServer.log("contacts: observer notification failed: " + failure);
        } finally {
            Binder.restoreCallingIdentity(identity);
        }
    }

    /** Registers cursor change notifications outside provider locks. */
    static void setNotificationUri(Cursor cursor, Context context, Uri uri) {
        if (cursor == null || context == null || uri == null) return;
        android.content.ContentResolver resolver = context.getContentResolver();
        if (resolver == null) return;
        long identity = Binder.clearCallingIdentity();
        try {
            cursor.setNotificationUri(resolver, uri);
        } catch (RuntimeException failure) {
            SystemServer.log("contacts: cursor notification registration failed: " + failure);
        } finally {
            Binder.restoreCallingIdentity(identity);
        }
    }

    static String parseLimit(Uri uri) {
        if (uri == null) return null;
        String limitParam = uri.getQueryParameter("limit");
        String offsetParam = uri.getQueryParameter("offset");
        if (limitParam == null && offsetParam == null) return null;
        if (limitParam == null) {
            throw new IllegalArgumentException("offset requires limit query parameter");
        }
        int limit;
        try {
            limit = Integer.parseInt(limitParam);
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException("Invalid limit parameter: " + limitParam, e);
        }
        if (limit < 0) {
            throw new IllegalArgumentException("Negative limit parameter: " + limitParam);
        }
        if (offsetParam == null) return Integer.toString(limit);
        int offset;
        try {
            offset = Integer.parseInt(offsetParam);
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException("Invalid offset parameter: " + offsetParam, e);
        }
        if (offset < 0) {
            throw new IllegalArgumentException("Negative offset parameter: " + offsetParam);
        }
        return offset + "," + limit;
    }

    static String combineSelection(String base, String clause) {
        if (clause == null || clause.isEmpty()) return base;
        if (base == null || base.trim().isEmpty()) return clause;
        return "(" + clause + ") AND (" + base + ")";
    }

    static String[] prependArgs(String[] base, String... prefix) {
        List<String> merged = new ArrayList<>((prefix == null ? 0 : prefix.length)
                + (base == null ? 0 : base.length));
        if (prefix != null) {
            for (String p : prefix) merged.add(p);
        }
        if (base != null) {
            for (String b : base) merged.add(b);
        }
        return merged.toArray(new String[0]);
    }
}
