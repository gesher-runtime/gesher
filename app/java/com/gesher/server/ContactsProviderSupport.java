/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.AppOpsManager;
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
 * Shared system-owned metadata, permission/AppOps/URI-grant enforcement,
 * phone-number matching, and package admission rules for the built-in
 * {@code com.android.providers.contacts} providers.
 */
final class ContactsProviderSupport {
    static final String PACKAGE = "com.android.providers.contacts";
    static final String CALL_LOG_AUTHORITY = "call_log";
    static final String CALL_LOG_CLASS = "com.android.providers.contacts.CallLogProvider";

    static final String READ_CALL_LOG = "android.permission.READ_CALL_LOG";
    static final String WRITE_CALL_LOG = "android.permission.WRITE_CALL_LOG";

    private static final int PHONE_MIN_MATCH = 7;

    private ContactsProviderSupport() { }

    static File dataDirectory() {
        return new File(Device.dataDir(), "system/contacts");
    }

    static boolean isSystemAuthority(String authority) {
        return CALL_LOG_AUTHORITY.equals(authority);
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

    static ProviderInfo callLogProviderInfo() {
        return providerInfo(CALL_LOG_CLASS, CALL_LOG_AUTHORITY, READ_CALL_LOG, WRITE_CALL_LOG);
    }

    static ProviderInfo providerInfo(String className, String authority,
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

    static PackageInfo packageInfo(long flags) {
        PackageInfo info = new PackageInfo();
        info.packageName = PACKAGE;
        info.applicationInfo = applicationInfo();
        info.versionName = (Build.VERSION.RELEASE != null && !Build.VERSION.RELEASE.isEmpty())
                ? Build.VERSION.RELEASE : "16";
        info.setLongVersionCode(Build.VERSION.SDK_INT);
        if ((flags & PackageManager.GET_PROVIDERS) != 0 && matchesProviderFlags(flags)) {
            info.providers = new ProviderInfo[] { callLogProviderInfo() };
        }
        return info;
    }

    /** Returns true if an APK attempts to claim the built-in contacts package or authorities. */
    static boolean hasBuiltInConflict(ApkPackage pkg) {
        if (pkg == null || pkg.parsed == null) return false;
        if (PACKAGE.equals(pkg.packageName)) return true;
        for (ParsedProvider provider : pkg.parsed.getProviders()) {
            String authorities = provider.getAuthority();
            if (authorities == null) continue;
            for (String authority : authorities.split(";")) {
                if (isSystemAuthority(authority)) return true;
            }
        }
        return false;
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

    /** Provider context wired to {@link UriGrants}, matching {@link DocumentsStorageProvider}. */
    static Context contextForProvider(UriGrants grants,
            ContentProviders.RegisteredProvider registration) {
        Context system = android.app.GesherSettingsContext.get();
        return new ContextWrapper(system) {
            @Override public String getPackageName() { return PACKAGE; }
            @Override public String getOpPackageName() { return PACKAGE; }
            @Override public ApplicationInfo getApplicationInfo() { return applicationInfo(); }
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

    /**
     * Authenticates {@link android.content.AttributionSource} and enforces URI grant,
     * runtime permission, and {@link AppOpsService} rules for Binder {@code ContentProvider}
     * permission hooks when the provider is hosted in {@code system_server}.
     */
    static int enforcePermissionInner(PackageManagerService pm, CallerIdentity callers,
            UriGrants grants, Uri uri, int uriModeFlag, String permission,
            android.content.AttributionSource source) {
        if (pm == null || callers == null) {
            if (Binder.getCallingPid() != Process.myPid()) {
                throw new SecurityException("Provider caller identity unavailable");
            }
            return android.content.PermissionChecker.PERMISSION_GRANTED;
        }
        if (Binder.getCallingPid() == Process.myPid() || callers.isSystem()) {
            return android.content.PermissionChecker.PERMISSION_GRANTED;
        }
        int pid = callers.callerPid();
        int uid = callers.callerUid();
        if (source == null || source.getUid() != uid
                || (source.getPid() != -1 && source.getPid() != pid)) {
            throw new SecurityException("Provider attribution must identify its Binder caller");
        }
        if (android.os.UserHandle.getUserId(uid) != 0) {
            throw new SecurityException("Provider access restricted to primary user");
        }
        callers.requirePackage(source.getPackageName());
        if (grants != null && uri != null && uriModeFlag != 0
                && grants.check(uri, pid, uid, uriModeFlag, 0) == PackageManager.PERMISSION_GRANTED) {
            return android.content.PermissionChecker.PERMISSION_GRANTED;
        }
        java.util.Set<String> renounced = source.getRenouncedPermissions();
        if (permission != null && renounced != null && renounced.contains(permission)) {
            throw new SecurityException("Permission renounced by caller attribution: " + permission);
        }
        enforce(pm, callers, grants, uri, uriModeFlag, permission);
        return android.content.PermissionChecker.PERMISSION_GRANTED;
    }

    /**
     * Enforces caller access via URI grant first, then runtime/install permission
     * and {@link AppOpsService} (so legacy apps with revoked AppOps are denied and
     * authorized accesses are noted).
     */
    static void enforce(PackageManagerService pm, CallerIdentity callers, UriGrants grants,
            Uri uri, int uriModeFlag, String permission) {
        int pid = Binder.getCallingPid();
        int uid = Binder.getCallingUid();
        if (pm == null || callers == null) {
            if (pid != Process.myPid()) {
                throw new SecurityException("Provider caller identity unavailable");
            }
            return;
        }
        if (pid == Process.myPid()) return;
        if (callers.isSystem()) return;
        if (grants != null && uri != null && uriModeFlag != 0
                && grants.check(uri, pid, uid, uriModeFlag, 0) == PackageManager.PERMISSION_GRANTED) {
            return;
        }
        String actual = callers.callerPackage();
        ApkPackage callerPkg = pm.get(actual);
        if (callerPkg == null || callerPkg.uid() != uid || pm.isRemovalPending(actual)) {
            throw new SecurityException("Unknown provider caller identity: " + actual);
        }
        if (permission != null) {
            if (pm.permissions().checkPermission(actual, permission) != PackageManager.PERMISSION_GRANTED) {
                throw new SecurityException("Permission Denial: requires " + permission);
            }
            int op = AppOpsManager.permissionToOpCode(permission);
            if (op != AppOpsManager.OP_NONE) {
                int mode;
                long identity = Binder.clearCallingIdentity();
                try {
                    mode = pm.appOps().noteOperation(op, uid, actual, null, false, null, false)
                            .getOpMode();
                } finally {
                    Binder.restoreCallingIdentity(identity);
                }
                if (mode != AppOpsManager.MODE_ALLOWED) {
                    throw new SecurityException("AppOps Denial: " + permission + " for " + actual);
                }
            }
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

    /**
     * Registers cursor change notifications outside provider locks under the caller's
     * identity so closing the cursor can unregister its observer. Only skips when no
     * {@code content} system service is reachable (e.g. an isolated no-binder fixture
     * with neither {@code $GESHER_BINDER} nor a cached/in-process {@code ServiceManager}).
     */
    static void setNotificationUri(Cursor cursor, Context context, Uri uri) {
        if (cursor == null || context == null || uri == null) return;
        android.content.ContentResolver resolver = context.getContentResolver();
        if (resolver == null || !hasContentService()) return;
        cursor.setNotificationUri(resolver, uri);
    }

    private static boolean hasContentService() {
        boolean hasServiceManager = false;
        String binder = System.getenv("GESHER_BINDER");
        if (binder != null && !binder.isEmpty()) hasServiceManager = true;
        try {
            java.lang.reflect.Field smField = android.os.ServiceManager.class.getDeclaredField("sServiceManager");
            smField.setAccessible(true);
            if (smField.get(null) != null) hasServiceManager = true;
        } catch (ReflectiveOperationException ignored) { }
        if (!hasServiceManager) {
            try {
                java.lang.reflect.Field cacheField = android.os.ServiceManager.class.getDeclaredField("sCache");
                cacheField.setAccessible(true);
                Object cache = cacheField.get(null);
                if (!(cache instanceof java.util.Map<?, ?> map)
                        || map.get(android.content.ContentResolver.CONTENT_SERVICE_NAME) == null) {
                    return false;
                }
            } catch (ReflectiveOperationException ignored) {
                return false;
            }
        }
        return android.os.ServiceManager.getService(android.content.ContentResolver.CONTENT_SERVICE_NAME) != null;
    }

    /**
     * Normalizes a phone string to dialable characters ({@code +} prefix if leading,
     * plus {@code 0-9}, {@code *}, {@code #}), returning {@code null} when no dialable
     * character is present.
     */
    static String normalizePhoneNumber(String number) {
        if (number == null) return null;
        StringBuilder sb = new StringBuilder(number.length());
        for (int i = 0; i < number.length(); i++) {
            char c = number.charAt(i);
            if (c == '+' && sb.length() == 0) {
                sb.append(c);
            } else if ((c >= '0' && c <= '9') || c == '*' || c == '#') {
                sb.append(c);
            }
        }
        if (sb.length() == 0 || (sb.length() == 1 && sb.charAt(0) == '+')) return null;
        return sb.toString();
    }

    /**
     * Compares two phone strings using Android {@code PhoneNumberUtils.compare} /
     * {@code PHONE_NUMBERS_EQUAL} loose semantics ({@code MIN_MATCH = 7}):
     * local trunk prefix {@code 0} matches an international {@code +CC}/{@code 00CC}
     * prefix, while short suffixes like {@code "1"} do not match longer numbers.
     */
    static boolean phoneNumbersEqualLoose(String a, String b) {
        String na = normalizePhoneNumber(a);
        String nb = normalizePhoneNumber(b);
        if (na == null || nb == null) return false;
        if (na.equals(nb)) return true;

        int matched = 0;
        int ia = na.length() - 1;
        int ib = nb.length() - 1;
        while (ia >= 0 && ib >= 0) {
            char ca = na.charAt(ia);
            char cb = nb.charAt(ib);
            if (ca == '+' || cb == '+') break;
            if (ca != cb) break;
            matched++;
            ia--;
            ib--;
        }
        String remA = na.substring(0, ia + 1);
        String remB = nb.substring(0, ib + 1);
        if (remA.isEmpty() && remB.isEmpty()) return true;
        if (matched < PHONE_MIN_MATCH) return false;
        return isIgnorablePhonePrefix(remA) && isIgnorablePhonePrefix(remB);
    }

    private static boolean isIgnorablePhonePrefix(String prefix) {
        if (prefix.isEmpty() || "0".equals(prefix)) return true;
        if (prefix.startsWith("+")) {
            int digits = prefix.length() - 1;
            return digits >= 1 && digits <= 3 && allAsciiDigits(prefix, 1);
        }
        if (prefix.startsWith("00")) {
            int digits = prefix.length() - 2;
            return digits >= 1 && digits <= 3 && allAsciiDigits(prefix, 2);
        }
        return false;
    }

    private static boolean allAsciiDigits(String s, int start) {
        for (int i = start; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c < '0' || c > '9') return false;
        }
        return true;
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
