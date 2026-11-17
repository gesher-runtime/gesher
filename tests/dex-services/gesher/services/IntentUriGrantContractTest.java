/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.app.IUriGrantsManager;
import android.content.ClipData;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.content.pm.ProviderInfo;
import android.net.Uri;
import android.os.Binder;
import android.os.IBinder;
import android.os.Process;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedPermissionImpl;
import com.android.internal.pm.pkg.component.ParsedProviderImpl;
import com.gesher.server.ActivityManagerService;
import com.gesher.server.ActivityTaskManagerService;
import com.gesher.server.ApkPackage;
import com.gesher.server.CallerIdentity;
import com.gesher.server.PackageManagerService;
import com.gesher.server.SharedLibraryGraph;
import com.gesher.server.StandIn;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import sun.misc.Unsafe;

/** Contract tests for F10: Intent URI grants and IBinder owner tokens. */
public final class IntentUriGrantContractTest {
    private static final String SENDER = "fixture.intent.sender";
    private static final String TARGET = "fixture.intent.target";
    private static final String OTHER = "fixture.intent.other";
    private static final int SENDER_UID = 10100;
    private static final int TARGET_UID = 10200;
    private static final int OTHER_UID = 10300;
    private static final String AUTHORITY = "fixture.intent.provider";

    private static Unsafe unsafe;
    private static int checks = 0;

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }

    private static void set(Object target, String name, Object value) throws Exception {
        Class<?> current = target.getClass();
        while (current != null) {
            try {
                Field f = current.getDeclaredField(name);
                f.setAccessible(true);
                f.set(target, value);
                return;
            } catch (NoSuchFieldException ignored) {
                current = current.getSuperclass();
            }
        }
        throw new NoSuchFieldException(name);
    }

    private static Object get(Object target, String name) throws Exception {
        Class<?> current = target.getClass();
        while (current != null) {
            try {
                Field f = current.getDeclaredField(name);
                f.setAccessible(true);
                return f.get(target);
            } catch (NoSuchFieldException ignored) {
                current = current.getSuperclass();
            }
        }
        throw new NoSuchFieldException(name);
    }

    private static ApkPackage createPackage(String name, int uid, boolean withProvider) throws Exception {
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        if (withProvider) {
            ParsedPermissionImpl rPerm = new ParsedPermissionImpl();
            rPerm.setName(name + ".READ"); rPerm.setPackageName(name);
            rPerm.setProtectionLevel(PermissionInfo.PROTECTION_NORMAL); parsed.addPermission(rPerm);
            ParsedPermissionImpl wPerm = new ParsedPermissionImpl();
            wPerm.setName(name + ".WRITE"); wPerm.setPackageName(name);
            wPerm.setProtectionLevel(PermissionInfo.PROTECTION_NORMAL); parsed.addPermission(wPerm);

            ParsedProviderImpl provider = new ParsedProviderImpl();
            provider.setName(name + ".Provider");
            provider.setPackageName(name);
            provider.setEnabled(true);
            provider.setExported(true);
            provider.setAuthority(AUTHORITY);
            provider.setGrantUriPermissions(true);
            provider.setReadPermission(name + ".READ");
            provider.setWritePermission(name + ".WRITE");
            parsed.addProvider(provider);
        }
        parsed.hideAsFinal();
        ApkPackage code = (ApkPackage) unsafe.allocateInstance(ApkPackage.class);
        set(code, "packageName", name);
        set(code, "parsed", parsed);
        set(code, "mUid", uid);
        set(code, "apkPath", "/fixture/" + name + "/base.apk");
        set(code, "mDataDir", "/fixture/" + name + "/data");
        set(code, "sharedLibraries", SharedLibraryGraph.EMPTY);
        return code;
    }

    public static void main(String[] args) throws Exception {
        contracts(true);
    }

    public static void contracts(boolean loadNative) throws Exception {
        if (loadNative) System.loadLibrary("android_runtime");
        Field singleton = Unsafe.class.getDeclaredField("theUnsafe");
        singleton.setAccessible(true);
        unsafe = (Unsafe) singleton.get(null);

        PackageManagerService pm = (PackageManagerService) unsafe.allocateInstance(PackageManagerService.class);
        Map<String, ApkPackage> pkgs = new LinkedHashMap<>();
        ApkPackage senderPkg = createPackage(SENDER, SENDER_UID, true);
        ApkPackage targetPkg = createPackage(TARGET, TARGET_UID, false);
        ApkPackage otherPkg = createPackage(OTHER, OTHER_UID, false);
        pkgs.put(SENDER, senderPkg);
        pkgs.put(TARGET, targetPkg);
        pkgs.put(OTHER, otherPkg);
        set(pm, "mPackages", pkgs);
        set(pm, "mRemoving", new LinkedHashMap<>());
        set(pm, "mPackageRemoval", new ArrayList<>());
        set(pm, "mPackageRemovalChecks", new ArrayList<>());
        Class<?> policy = Class.forName("com.gesher.server.PermissionPolicy");
        Constructor<?> policyConstructor = policy.getDeclaredConstructor(PackageManagerService.class);
        policyConstructor.setAccessible(true);
        set(pm, "mPermissions", policyConstructor.newInstance(pm));

        ActivityTaskManagerService atm = (ActivityTaskManagerService) unsafe.allocateInstance(ActivityTaskManagerService.class);
        set(atm, "mPm", pm);
        set(atm, "mProcesses", new ArrayList<>());

        ActivityManagerService am = (ActivityManagerService) unsafe.allocateInstance(ActivityManagerService.class);
        set(am, "mPm", pm);
        set(am, "mAtm", atm);
        set(atm, "mAm", am);

        Constructor<?> grantsCtor = Class.forName("com.gesher.server.UriGrants")
                .getDeclaredConstructor(PackageManagerService.class, ActivityTaskManagerService.class);
        grantsCtor.setAccessible(true);
        Object grants = grantsCtor.newInstance(pm, atm);
        Object providers = unsafe.allocateInstance(Class.forName("com.gesher.server.ContentProviders"));
        set(providers, "mPm", pm);
        set(providers, "mUriGrants", grants);
        set(am, "mProviders", providers);
        set(am, "mCallers", new CallerIdentity(pm, atm));

        Constructor<?> ugmCtor = Class.forName("com.gesher.server.UriGrantsManagerService")
                .getDeclaredConstructor(PackageManagerService.class, ActivityTaskManagerService.class, ActivityManagerService.class);
        ugmCtor.setAccessible(true);
        IUriGrantsManager ugm = (IUriGrantsManager) ugmCtor.newInstance(pm, atm, am);

        Uri testUri = Uri.parse("content://" + AUTHORITY + "/document/123");
        Uri childUri = Uri.parse("content://" + AUTHORITY + "/document/123/child");
        int read = Intent.FLAG_GRANT_READ_URI_PERMISSION;
        int write = Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
        int prefix = Intent.FLAG_GRANT_PREFIX_URI_PERMISSION;

        // Test 1: Initially target has no access
        Method checkMethod = grants.getClass().getDeclaredMethod("check", Uri.class, int.class, int.class, int.class, int.class);
        checkMethod.setAccessible(true);
        int initialCheck = (Integer) checkMethod.invoke(grants, testUri, 500, TARGET_UID, read, 0);
        check(initialCheck == PackageManager.PERMISSION_DENIED, "Initial access must be denied");

        // Test 2: grantUriPermissionFromOwner with owner token
        Method newOwner = grants.getClass().getDeclaredMethod("newOwner");
        newOwner.setAccessible(true);
        IBinder owner1 = (IBinder) newOwner.invoke(grants);
        Method grantFromOwnerMethod = grants.getClass().getDeclaredMethod("grantFromOwner",
                IBinder.class, int.class, String.class, Uri.class, int.class, int.class, int.class);
        grantFromOwnerMethod.setAccessible(true);

        grantFromOwnerMethod.invoke(grants, owner1, SENDER_UID, TARGET, testUri, read, 0, 0);

        Method canAccessMethod = grants.getClass().getDeclaredMethod("canAccess", ApkPackage.class, Uri.class, int.class, int.class);
        canAccessMethod.setAccessible(true);
        boolean targetAccess = (Boolean) canAccessMethod.invoke(grants, targetPkg, testUri, read, 0);
        check(targetAccess, "Target package must have read access after owner grant");

        boolean targetWrite = (Boolean) canAccessMethod.invoke(grants, targetPkg, testUri, write, 0);
        check(!targetWrite, "Target package must not have write access when only read was granted");

        boolean otherAccess = (Boolean) canAccessMethod.invoke(grants, otherPkg, testUri, read, 0);
        check(!otherAccess, "Unrelated package must not have access");

        // Test 3: checkGrantUriPermission_ignoreNonSystem
        Method checkGrantMethod = grants.getClass().getDeclaredMethod("checkGrantUriPermission_ignoreNonSystem",
                int.class, String.class, Uri.class, int.class, int.class);
        checkGrantMethod.setAccessible(true);

        int checkSystem = (Integer) checkGrantMethod.invoke(grants, SENDER_UID, TARGET, testUri, read, 0);
        check(checkSystem == TARGET_UID, "checkGrantUriPermission_ignoreNonSystem must return target UID for valid grant");

        try {
            checkGrantMethod.invoke(grants, OTHER_UID, TARGET, testUri, read, 0);
            throw new AssertionError("Unauthorized grant query accepted");
        } catch (java.lang.reflect.InvocationTargetException e) {
            check(e.getCause() instanceof SecurityException, "Unauthorized grant query throws SecurityException");
        }

        // Test 4: Multiple owners on same URI with different modes
        IBinder owner2 = (IBinder) newOwner.invoke(grants);
        grantFromOwnerMethod.invoke(grants, owner2, SENDER_UID, TARGET, testUri, write, 0, 0);
        check((Boolean) canAccessMethod.invoke(grants, targetPkg, testUri, read, 0),
                "Target must have read from owner1");
        check((Boolean) canAccessMethod.invoke(grants, targetPkg, testUri, write, 0),
                "Target must have write from owner2");

        check((Boolean) canAccessMethod.invoke(grants, targetPkg, testUri, read | write, 0),
                "Read and write owners jointly confer both requested modes");

        // Test 5: Revocation of owner1 revokes read, but keeps owner2 write
        Method revokeByOwnerMethod = grants.getClass().getDeclaredMethod("revokeByOwner", IBinder.class);
        revokeByOwnerMethod.setAccessible(true);
        revokeByOwnerMethod.invoke(grants, owner1);

        check(!(Boolean) canAccessMethod.invoke(grants, targetPkg, testUri, read, 0),
                "Target must lose read access after owner1 is revoked");
        check((Boolean) canAccessMethod.invoke(grants, targetPkg, testUri, write, 0),
                "Target must retain write access from owner2");

        // Test 6: Revocation of owner2 revokes write as well
        revokeByOwnerMethod.invoke(grants, owner2);
        check(!(Boolean) canAccessMethod.invoke(grants, targetPkg, testUri, write, 0),
                "Target must lose write access after owner2 is revoked");

        // Test 7: Prefix owner grant
        IBinder ownerPrefix = (IBinder) newOwner.invoke(grants);
        grantFromOwnerMethod.invoke(grants, ownerPrefix, SENDER_UID, TARGET, testUri, read | prefix, 0, 0);
        check((Boolean) canAccessMethod.invoke(grants, targetPkg, childUri, read, 0),
                "Target must have read access to child URI under prefix grant");
        revokeByOwnerMethod.invoke(grants, ownerPrefix);
        check(!(Boolean) canAccessMethod.invoke(grants, targetPkg, childUri, read, 0),
                "Child URI access must be revoked when prefix owner is revoked");

        // Test 8: ActivityCallerState contentUris extraction
        Intent testIntent = new Intent();
        testIntent.setData(testUri);
        ClipData clip = ClipData.newRawUri("label", childUri);
        testIntent.setClipData(clip);

        Class<?> callerStateClass = Class.forName("com.gesher.server.ActivityCallerState");
        Method contentUrisMethod = callerStateClass.getDeclaredMethod("contentUris", Intent.class);
        contentUrisMethod.setAccessible(true);
        @SuppressWarnings("unchecked")
        java.util.Set<Uri> extractedUris = (java.util.Set<Uri>) contentUrisMethod.invoke(null, testIntent);
        check(extractedUris.contains(testUri), "contentUris must extract intent data URI");
        check(extractedUris.contains(childUri), "contentUris must extract ClipData URI");

        try {
            grantFromOwnerMethod.invoke(grants, new Binder(), SENDER_UID, TARGET, testUri, read, 0, 0);
            throw new AssertionError("Unknown owner accepted");
        } catch (java.lang.reflect.InvocationTargetException e) {
            check(e.getCause() instanceof IllegalArgumentException, "Unknown external owner rejected");
        }
        Method prepare = grants.getClass().getDeclaredMethod("prepareIntent", int.class, String.class, Intent.class);
        prepare.setAccessible(true);
        Intent extrasOnly = new Intent().addFlags(read);
        extrasOnly.putExtra(Intent.EXTRA_STREAM, testUri);
        check(((List<?>) prepare.invoke(grants, SENDER_UID, TARGET, extrasOnly)).isEmpty(),
                "EXTRA_STREAM discovery does not grant access without ClipData migration");
        Intent invalidBatch = new Intent().setData(testUri).addFlags(read);
        invalidBatch.setClipData(ClipData.newRawUri("denied", Uri.parse("content://" + AUTHORITY + "/second")));
        // Preparation is side-effect free even on success.
        check(((List<?>) prepare.invoke(grants, SENDER_UID, TARGET, invalidBatch)).size() == 2,
                "Intent data and ClipData validated as a batch");
        check(!(Boolean) canAccessMethod.invoke(grants, targetPkg, testUri, read, 0),
                "Preparing a batch never publishes a partial grant");
        check(((List<?>) prepare.invoke(grants, Process.SYSTEM_UID, TARGET, invalidBatch)).isEmpty(),
                "System cannot delegate arbitrary provider access");
        Intent file = new Intent().setData(Uri.parse("file:///tmp/no-grant")).addFlags(read);
        check(((List<?>) prepare.invoke(grants, SENDER_UID, TARGET, file)).isEmpty(),
                "Non-content intent data creates no URI grant");

        Intent deniedBatch = new Intent().setData(testUri).addFlags(read);
        deniedBatch.setClipData(ClipData.newRawUri("cross-user",
                Uri.parse("content://1@" + AUTHORITY + "/denied")));
        try {
            prepare.invoke(grants, SENDER_UID, TARGET, deniedBatch);
            throw new AssertionError("Cross-user batch accepted");
        } catch (java.lang.reflect.InvocationTargetException e) {
            check(e.getCause() instanceof SecurityException, "Invalid second URI rejects whole batch");
        }
        check(!(Boolean) canAccessMethod.invoke(grants, targetPkg, testUri, read, 0),
                "Failed batch never publishes first valid URI");

        Class<?> servicesType = Class.forName("com.gesher.server.ActiveServices");
        Object services = unsafe.allocateInstance(servicesType);
        set(services, "mAtm", atm); set(services, "mPm", pm);
        set(services, "mCallers", new CallerIdentity(pm, atm));
        Class<?> recordType = Class.forName("com.gesher.server.ActiveServices$ServiceRecord");
        Constructor<?> recordConstructor = recordType.getDeclaredConstructor(
                android.content.pm.ServiceInfo.class, ApkPackage.class);
        recordConstructor.setAccessible(true);
        android.content.pm.ServiceInfo serviceInfo = new android.content.pm.ServiceInfo();
        serviceInfo.packageName = TARGET; serviceInfo.name = TARGET + ".Service";
        Object record = recordConstructor.newInstance(serviceInfo, targetPkg);
        Method done = servicesType.getDeclaredMethod("serviceDoneExecuting", IBinder.class,
                int.class, int.class, int.class); done.setAccessible(true);
        Method publish = grants.getClass().getDeclaredMethod("publish", IBinder.class, List.class);
        publish.setAccessible(true);
        @SuppressWarnings("unchecked") Map<Integer, IBinder> serviceOwners =
                (Map<Integer, IBinder>) get(record, "startGrantOwners");
        IBinder startOwner = new Binder();
        publish.invoke(grants, startOwner, prepare.invoke(grants, SENDER_UID, TARGET,
                new Intent().setData(testUri).addFlags(read)));
        serviceOwners.put(1, startOwner);
        done.invoke(services, record, 1, 1, android.app.Service.START_STICKY);
        check(serviceOwners.isEmpty(), "Completed sticky start retires its owner");
        check(!(Boolean) canAccessMethod.invoke(grants, targetPkg, testUri, read, 0),
                "Completed service start revokes its URI permissions");
        publish.invoke(grants, startOwner, prepare.invoke(grants, SENDER_UID, TARGET,
                new Intent().setData(testUri).addFlags(read)));
        serviceOwners.put(2, startOwner);
        done.invoke(services, record, 1, 2, android.app.Service.START_REDELIVER_INTENT);
        check(serviceOwners.size() == 1 && (Boolean) canAccessMethod.invoke(
                grants, targetPkg, testUri, read, 0), "Redelivery start retains its grant until explicit stop");
        Method clearStarts = servicesType.getDeclaredMethod("clearStartGrants", recordType, int.class);
        clearStarts.setAccessible(true); clearStarts.invoke(services, record, 2);
        check(serviceOwners.isEmpty() && !(Boolean) canAccessMethod.invoke(
                grants, targetPkg, testUri, read, 0), "Stop through startId revokes retained redelivery grant");

        System.out.println("IntentUriGrantContractTest passed: " + checks + " checks");
    }
}
