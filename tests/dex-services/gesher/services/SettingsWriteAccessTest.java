/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.app.AppOpsManager;
import android.app.IApplicationThread;
import android.content.ContentProvider;
import android.content.ContentResolver;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.ContentValues;
import android.content.IContentProvider;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ProviderInfo;
import android.database.ContentObserver;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import com.gesher.server.ApkPackage;
import com.gesher.server.AppOpsService;
import com.gesher.server.PackageManagerService;
import java.io.File;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import static gesher.services.ClipboardFixture.*;

/** Real caller/permission/AppOps/storage policies with explicit APK and Binder identity inputs. */
public final class SettingsWriteAccessTest {
    private static int checks;
    private static void check(boolean value, String message) { if (!value) throw new AssertionError(message); checks++; }
    private static Bundle value(String text) { Bundle result = new Bundle(); result.putString("value", text); return result; }
    private static final class ProviderContext extends ContextWrapper {
        final ContentResolver resolver = new ContentResolver(this) {
            @Override protected IContentProvider acquireProvider(Context c, String name) { throw new AssertionError("Unexpected provider lookup"); }
            @Override protected IContentProvider acquireUnstableProvider(Context c, String name) { throw new AssertionError("Unexpected provider lookup"); }
            @Override public boolean releaseProvider(IContentProvider p) { return true; }
            @Override public boolean releaseUnstableProvider(IContentProvider p) { return true; }
            @Override public void unstableProviderDied(IContentProvider p) { throw new AssertionError("Unexpected provider death"); }
            @Override public void notifyChange(Uri uri, ContentObserver observer) { notifications++; }
        };
        int notifications;
        ProviderContext() { super(null); }
        @Override public String getPackageName() { return "android"; }
        @Override public String getOpPackageName() { return "android"; }
        @Override public int getUserId() { return 0; }
        @Override public ApplicationInfo getApplicationInfo() { return new ApplicationInfo(); }
        @Override public Object getSystemService(String name) { return null; }
        @Override public ContentResolver getContentResolver() { return resolver; }
    }
    private static void denied(Checked work) throws Exception {
        try { work.run(); } catch (SecurityException expected) { checks++; return; }
        throw new AssertionError("Unauthorized settings mutation was accepted");
    }
    private static void as(ApkPackage pkg, int pid, Checked work) throws Exception {
        long identity = Binder.clearCallingIdentity();
        try { Binder.restoreCallingIdentity(((long)pkg.uid() << 32) | pid); work.run(); }
        finally { Binder.restoreCallingIdentity(identity); }
    }
    public static int run(boolean nativeFramework) throws Exception {
        int remote = run(nativeFramework, false);
        return remote + run(nativeFramework, true);
    }
    @SuppressWarnings("unchecked") private static int run(boolean nativeFramework, boolean inProcess) throws Exception {
        checks = 0;
        ApkPackage ordinary = pkg("fixture.settings.ordinary", 12811, (byte)7, Manifest.permission.WRITE_SETTINGS);
        ApkPackage peer = pkg("fixture.settings.peer", 12812, (byte)8, Manifest.permission.WRITE_SETTINGS);
        ApkPackage signed = pkg("fixture.settings.signed", 12813, (byte)42, Manifest.permission.WRITE_SETTINGS);
        ApkPackage secure = pkg("fixture.settings.secure", 12814, (byte)42, Manifest.permission.WRITE_SECURE_SETTINGS);
        ApkPackage platform = pkg("android", android.os.Process.SYSTEM_UID, (byte)42);
        PackageManagerService pm = (PackageManagerService)make(PackageManagerService.class,
                new Class<?>[]{Collection.class, boolean.class}, List.of(ordinary, peer, signed, secure, platform), false);
        set(pm, "mPackageEventSender", (java.util.function.Consumer<Intent>)event -> { });
        Object atm = make(type("ActivityTaskManagerService"), new Class<?>[]{PackageManagerService.class}, pm);
        make(type("ActivityManagerService"), new Class<?>[]{atm.getClass(), PackageManagerService.class, type("NotificationManagerService")}, atm, pm, null);
        if (!nativeFramework) {
            // Only the pinned Android16r4 manifest metadata is supplied on the host.
            Object catalog = get(pm.permissions(), "mCatalog");
            Map<String, PermissionInfo> definitions = (Map<String, PermissionInfo>)get(catalog, "mFramework");
            PermissionInfo settings = new PermissionInfo(); settings.name = Manifest.permission.WRITE_SETTINGS; settings.packageName = "android";
            settings.protectionLevel = PermissionInfo.PROTECTION_SIGNATURE | PermissionInfo.PROTECTION_FLAG_PREINSTALLED
                    | PermissionInfo.PROTECTION_FLAG_APPOP | PermissionInfo.PROTECTION_FLAG_PRE23 | PermissionInfo.PROTECTION_FLAG_ROLE;
            definitions.put(settings.name, settings);
            PermissionInfo privileged = new PermissionInfo(); privileged.name = Manifest.permission.WRITE_SECURE_SETTINGS; privileged.packageName = "android";
            privileged.protectionLevel = PermissionInfo.PROTECTION_SIGNATURE | PermissionInfo.PROTECTION_FLAG_PRIVILEGED
                    | PermissionInfo.PROTECTION_FLAG_DEVELOPMENT | PermissionInfo.PROTECTION_FLAG_ROLE | PermissionInfo.PROTECTION_FLAG_INSTALLER;
            definitions.put(privileged.name, privileged); set(catalog, "mLoaded", true);
        }
        List<ApkPackage> apps = List.of(ordinary, peer, signed, secure);
        // Run the same permission/AppOp/CRUD cases for an attached remote app and
        // for an ordinary app sharing the provider's process, as standalone does.
        int basePid = inProcess ? android.os.Process.myPid() : 887810;
        for (int i = 0; i < apps.size(); i++) {
            ApkPackage app = apps.get(i); int pid = basePid + i;
            as(app, pid, () -> {
                IApplicationThread thread = new IApplicationThread.Default() {
                    final Binder token = new Binder(); @Override public android.os.IBinder asBinder() { return token; }
                };
                Object process = make(type("ActivityTaskManagerService$ProcessRecord"),
                        new Class<?>[]{atm.getClass(), int.class, IApplicationThread.class}, atm, pid, thread);
                set(process, "packageName", app.packageName); set(process, "processName", app.packageName);
                set(process, "packageCode", app); set(process, "ready", true);
                ((List<Object>)get(atm, "mProcesses")).add(process);
            });
        }
        Object previous = field(type("SettingsProvider"), "sInstance").get(null);
        File file = File.createTempFile("settings-write-access-", ".bin"); file.delete();
        ContentProvider provider = (ContentProvider)make(type("SettingsProvider"),
                new Class<?>[]{PackageManagerService.class, atm.getClass(), File.class}, pm, atm, file);
        ProviderContext context = new ProviderContext();
        provider.attachInfo(context, (ProviderInfo)get(provider, "mInfo"));
        AppOpsService ops = pm.appOps();
        Uri table = Uri.parse("content://settings/system"), item = Uri.withAppendedPath(table, "access");
        try {
            check(pm.permissions().checkPermission(ordinary.packageName, Manifest.permission.WRITE_SETTINGS) == PackageManager.PERMISSION_DENIED,
                    "Manifest alone must not grant ordinary signature permission");
            as(ordinary, basePid, () -> denied(() -> provider.call("PUT_system", "access", value("denied-default"))));
            check(context.notifications == 0, "Denied mutation must not notify observers");
            ops.setMode(AppOpsManager.OP_WRITE_SETTINGS, ordinary.uid(), ordinary.packageName, AppOpsManager.MODE_ALLOWED);
            as(ordinary, basePid, () -> provider.call("PUT_system", "access", value("allowed")));
            check("allowed".equals(provider.call("GET_system", "access", null).getString("value")), "Real allowed AppOp must permit system mutation");
            check(pm.permissions().checkPermission(ordinary.packageName, Manifest.permission.WRITE_SETTINGS) == PackageManager.PERMISSION_DENIED,
                    "Special access must not fabricate a signature grant");
            as(ordinary, basePid, () -> {
                ContentValues values = new ContentValues(); values.put("name", "insert-access"); values.put("value", "created");
                check(provider.insert(table, values) != null, "Allowed special access covers insert");
                values.put("name", "access"); values.put("value", "updated");
                check(provider.update(item, values, null, null) == 1, "Allowed special access covers update");
                check(provider.delete(item, null, null) == 1, "Allowed special access covers delete");
                denied(() -> provider.call("PUT_secure", "secure-access", value("forbidden")));
                denied(() -> provider.call("PUT_global", "global-access", value("forbidden")));
            });
            for (int mode : new int[]{AppOpsManager.MODE_IGNORED, AppOpsManager.MODE_ERRORED, AppOpsManager.MODE_DEFAULT}) {
                ops.setMode(AppOpsManager.OP_WRITE_SETTINGS, ordinary.uid(), ordinary.packageName, mode);
                as(ordinary, basePid, () -> denied(() -> provider.call("PUT_system", "access", value("denied"))));
            }
            check(provider.call("GET_system", "access", null).getString("value") == null, "Denied writes must not republish deleted state");
            // The authenticated PID and UID must still identify the same installed APK.
            ops.setMode(AppOpsManager.OP_WRITE_SETTINGS, ordinary.uid(), ordinary.packageName, AppOpsManager.MODE_ALLOWED);
            as(peer, basePid, () -> denied(() -> provider.call("PUT_system", "spoofed", value("forbidden"))));
            as(ordinary, basePid + 99, () -> denied(() -> provider.call("PUT_system", "unknown", value("forbidden"))));
            ops.setMode(AppOpsManager.OP_WRITE_SETTINGS, ordinary.uid(), peer.packageName, AppOpsManager.MODE_ALLOWED);
            as(peer, basePid + 1, () -> denied(() -> provider.call("PUT_system", "mismatched-mode", value("forbidden"))));
            check(pm.permissions().checkPermission(signed.packageName, Manifest.permission.WRITE_SETTINGS) == PackageManager.PERMISSION_GRANTED,
                    "Signed fixture holds actual manifest permission through signer equality");
            as(signed, basePid + 2, () -> provider.call("PUT_system", "signature", value("default")));
            ops.setMode(AppOpsManager.OP_WRITE_SETTINGS, signed.uid(), signed.packageName, AppOpsManager.MODE_IGNORED);
            as(signed, basePid + 2, () -> denied(() -> provider.call("PUT_system", "signature", value("ignored"))));
            check("default".equals(provider.call("GET_system", "signature", null).getString("value")), "Ignored mode must override WRITE_SETTINGS fallback");
            ops.setMode(AppOpsManager.OP_WRITE_SETTINGS, secure.uid(), secure.packageName, AppOpsManager.MODE_IGNORED);
            as(secure, basePid + 3, () -> {
                provider.call("PUT_system", "privileged", value("system"));
                provider.call("PUT_secure", "privileged", value("secure"));
                provider.call("PUT_global", "privileged", value("global"));
            });
            for (String name : new String[]{"system", "secure", "global"}) check(name.equals(provider.call("GET_" + name, "privileged", null).getString("value")), "Secure permission authority " + name);
        } finally {
            field(type("SettingsProvider"), "sInstance").set(null, previous);
            ScheduledThreadPoolExecutor events = (ScheduledThreadPoolExecutor)get(ops, "mEvents"); events.shutdownNow();
            check(events.awaitTermination(5, TimeUnit.SECONDS), "Owned AppOps callbacks did not retire");
            check(!file.exists() || file.delete(), "Owned settings fixture file was not released");
        }
        return checks;
    }
}
