/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ContentProviderHolder;
import android.content.AttributionSource;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.IContentProvider;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Process;
import com.android.internal.pm.pkg.component.ParsedProvider;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * Shared system-owned metadata, caller authentication and URI-grant context,
 * and package admission rules for built-in contacts and call-log providers.
 */
final class ContactsProviderSupport {
    static final String PACKAGE = "android";
    static final String CALL_LOG_AUTHORITY = "call_log";
    static final String CALL_LOG_CLASS = "com.android.providers.contacts.CallLogProvider";

    static final String READ_CALL_LOG = "android.permission.READ_CALL_LOG";
    static final String WRITE_CALL_LOG = "android.permission.WRITE_CALL_LOG";

    private ContactsProviderSupport() { }

    static File dataDirectory() {
        return new File(Device.dataDir(), "system/contacts");
    }

    static boolean isSystemAuthority(String authority) {
        return CALL_LOG_AUTHORITY.equals(authority);
    }

    static ProviderInfo callLogProviderInfo() {
        ProviderInfo info = providerInfo(CALL_LOG_CLASS, CALL_LOG_AUTHORITY,
                READ_CALL_LOG, WRITE_CALL_LOG);
        // Unlike ContactsProvider, Android's CallLog does not support URI grants.
        info.grantUriPermissions = false;
        return info;
    }

    static ProviderInfo providerInfo(String className, String authority,
            String readPermission, String writePermission) {
        ApplicationInfo ai = PackageManagerService.platformApplication();
        ProviderInfo info = new ProviderInfo();
        info.applicationInfo = ai;
        info.packageName = PACKAGE;
        info.name = className;
        info.processName = "system";
        info.authority = authority;
        info.readPermission = readPermission;
        info.writePermission = writePermission;
        info.grantUriPermissions = true;
        info.exported = true;
        info.enabled = true;
        info.multiprocess = false;
        info.flags = ProviderInfo.FLAG_SINGLE_USER;
        return info;
    }

    static boolean matchesProviderFlags(long flags) {
        long directBoot = flags & (PackageManager.MATCH_DIRECT_BOOT_AWARE
                | PackageManager.MATCH_DIRECT_BOOT_UNAWARE);
        return directBoot != PackageManager.MATCH_DIRECT_BOOT_AWARE;
    }

    static ProviderInfo providerForAuthority(String authority, long flags) {
        if (!matchesProviderFlags(flags)) return null;
        if (CALL_LOG_AUTHORITY.equals(authority)) return callLogProviderInfo();
        return null;
    }

    static ProviderInfo providerForComponent(String className, long flags) {
        if (!matchesProviderFlags(flags)) return null;
        if (CALL_LOG_CLASS.equals(className)) return callLogProviderInfo();
        return null;
    }

    /** Returns true if an APK attempts to claim built-in contacts authorities. */
    static boolean hasBuiltInConflict(ApkPackage pkg) {
        if (pkg == null || pkg.parsed == null) return false;
        for (ParsedProvider provider : pkg.parsed.getProviders()) {
            String authorities = provider.getAuthority();
            if (authorities == null) continue;
            for (String authority : authorities.split(";")) {
                if (isSystemAuthority(authority)) return true;
            }
        }
        return false;
    }

    /** Rejects any APK attempting to claim a built-in contacts authority. */
    static void checkPackageAdmission(ApkPackage pkg)
            throws android.content.pm.PackageParser.PackageParserException {
        if (pkg == null || pkg.parsed == null) return;
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

    /** Provider context wired to {@link UriGrants}, matching {@link DocumentsStorageProvider}. */
    static Context contextForProvider(Context base, UriGrants grants,
            ContentProviders.RegisteredProvider registration) {
        return new ContextWrapper(base) {
            @Override public int checkUriPermission(Uri uri, int pid, int uid, int modes) {
                return grants == null ? PackageManager.PERMISSION_DENIED
                        : grants.check(uri, pid, uid, modes, 0);
            }
            @Override public void grantUriPermission(String target, Uri uri, int modes) {
                if (grants != null && registration != null) {
                    grants.grantFromProvider(registration, target, uri, modes);
                }
            }
            @Override public void revokeUriPermission(Uri uri, int modes) {
                if (grants != null && registration != null) {
                    grants.revokeFromProvider(registration, uri, modes);
                }
            }
            @Override public void revokeUriPermission(String target, Uri uri, int modes) {
                if (grants != null && registration != null) {
                    grants.revokeFromProvider(registration, target, uri, modes);
                }
            }
        };
    }

    static ContentProviderHolder holder(ProviderInfo info, IContentProvider transport) {
        ContentProviderHolder h = new ContentProviderHolder(info);
        h.provider = transport;
        h.noReleaseNeeded = true;
        return h;
    }

    static void authenticate(CallerIdentity callers, AttributionSource source) {
        if (Binder.getCallingPid() == Process.myPid()) return;
        if (callers == null) throw new SecurityException("Provider caller identity unavailable");
        if (source == null || source.getUid() != callers.callerUid()
                || (source.getPid() != -1 && source.getPid() != callers.callerPid())) {
            throw new SecurityException("Provider attribution must identify its Binder caller");
        }
        callers.requirePackage(source.getPackageName());
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

    static void setNotificationUri(Cursor cursor, Context context, Uri uri) {
        cursor.setNotificationUri(context.getContentResolver(), uri);
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
