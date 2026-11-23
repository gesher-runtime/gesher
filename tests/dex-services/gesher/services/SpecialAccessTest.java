/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.app.AppOpsManager;
import android.app.IApplicationThread;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import com.android.internal.app.IAppOpsCallback;
import com.gesher.server.ApkPackage;
import com.gesher.server.AppOpsService;
import com.gesher.server.PackageManagerService;
import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Supplier;
import static gesher.services.ClipboardFixture.*;

/**
 * The Control Center's special access switches (SpecialAccess) over the real PM, permission
 * policy, AppOps ledger and gesher service transaction. Only APK metadata, Binder caller
 * identities and (off ART) the platform manifest's permission definitions are fixtures.
 */
public final class SpecialAccessTest {
    private static int checks;
    private static final String APP = "fixture.access.app", PLAIN = "fixture.access.plain";
    private static final int APP_UID = 12901, PLAIN_UID = 12902, APP_PID = 889010;
    private static final String INSTALLER = "fixture.access.installer";
    private static final int INSTALLER_UID = 12903;
    private static final String FILES = AppOpsManager.OPSTR_MANAGE_EXTERNAL_STORAGE;

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private static void rejects(Class<? extends Throwable> expected, Checked work) throws Exception {
        try { work.run(); }
        catch (Throwable failure) {
            if (failure instanceof InvocationTargetException wrapped) failure = wrapped.getCause();
            if (expected.isInstance(failure)) { checks++; return; }
            throw new AssertionError("Expected " + expected.getSimpleName() + ", got " + failure, failure);
        }
        throw new AssertionError("Expected " + expected.getSimpleName());
    }
    private static void as(int uid, int pid, Checked work) throws Exception {
        long identity = Binder.clearCallingIdentity();
        try { Binder.restoreCallingIdentity(((long)uid << 32) | pid); work.run(); }
        finally { Binder.restoreCallingIdentity(identity); }
    }
    private static Object invoke(Class<?> type, String name, Object... args) throws Exception {
        for (Method method : type.getDeclaredMethods()) {
            if (method.getName().equals(name) && method.getParameterCount() == args.length) {
                method.setAccessible(true);
                try { return method.invoke(null, args); }
                catch (InvocationTargetException e) {
                    if (e.getCause() instanceof Exception cause) throw cause;
                    throw e;
                }
            }
        }
        throw new NoSuchMethodException(name);
    }
    @SuppressWarnings("unchecked")
    private static List<String> rows(PackageManagerService pm, String pkg) throws Exception {
        Class<?> access = type("SpecialAccess");
        return List.of((String[])invoke(access, "rows", invoke(access, "query", pm, pkg)));
    }
    private static String setAccess(PackageManagerService pm, String pkg, String op, boolean allow) throws Exception {
        Object entry = invoke(type("SpecialAccess"), "set", pm, pkg, op, allow);
        Method row = entry.getClass().getDeclaredMethod("row"); row.setAccessible(true);
        return (String)row.invoke(entry);
    }

    private static final class Watcher extends IAppOpsCallback.Default {
        final Binder token = new Binder(); final AtomicInteger calls = new AtomicInteger();
        volatile int lastOp = -1;
        @Override public IBinder asBinder() { return token; }
        @Override public void opChanged(int op, int uid, String pkg, String device) {
            if (uid == APP_UID && APP.equals(pkg)) {
                lastOp = op;
                if (op == AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE) calls.incrementAndGet();
            }
        }
    }
    private static void awaitCount(AtomicInteger count, int expected) throws Exception {
        long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (count.get() < expected && System.nanoTime() < end) Thread.sleep(5);
        check(count.get() == expected, "Expected " + expected + " app-op callbacks, got " + count.get());
    }

    /** What the framework's check answers in the app (Environment, Settings, MediaStore...). */
    private static boolean appSees(PackageManagerService pm, AppOpsService ops, int op, String permission) throws Exception {
        boolean[] result = new boolean[1];
        as(APP_UID, APP_PID, () -> {
            int mode = ops.checkOperation(op, APP_UID, APP);
            result[0] = mode == AppOpsManager.MODE_DEFAULT
                    ? pm.permissions().checkPermission(APP, permission) == PackageManager.PERMISSION_GRANTED
                    : mode == AppOpsManager.MODE_ALLOWED;
        });
        return result[0];
    }

    private static final String[][] DEFINITIONS = {
            {Manifest.permission.MANAGE_EXTERNAL_STORAGE, "appop|preinstalled"},
            {Manifest.permission.SYSTEM_ALERT_WINDOW, "appop|pre23|development"},
            {Manifest.permission.REQUEST_INSTALL_PACKAGES, "appop"},
            {Manifest.permission.WRITE_SETTINGS, "appop|preinstalled|pre23|role"},
            {Manifest.permission.PACKAGE_USAGE_STATS, "appop|privileged|development"},
            {Manifest.permission.MANAGE_MEDIA, "appop|preinstalled"},
            {Manifest.permission.INSTALL_PACKAGES, "privileged"},
    };

    @SuppressWarnings("unchecked")
    public static int run(boolean nativeFramework) throws Exception {
        checks = 0;
        String[] requested = {Manifest.permission.MANAGE_EXTERNAL_STORAGE, Manifest.permission.SYSTEM_ALERT_WINDOW,
                Manifest.permission.REQUEST_INSTALL_PACKAGES, Manifest.permission.WRITE_SETTINGS,
                Manifest.permission.PACKAGE_USAGE_STATS, Manifest.permission.MANAGE_MEDIA};
        ApkPackage app = pkg(APP, APP_UID, (byte)17, requested);
        ApkPackage plain = pkg(PLAIN, PLAIN_UID, (byte)18);
        // An installer signed like the platform holds INSTALL_PACKAGES (signature|privileged).
        ApkPackage platform = pkg("android", android.os.Process.SYSTEM_UID, (byte)42);
        ApkPackage installer = pkg(INSTALLER, INSTALLER_UID, (byte)42,
                Manifest.permission.REQUEST_INSTALL_PACKAGES, Manifest.permission.INSTALL_PACKAGES);
        PackageManagerService pm = (PackageManagerService)make(PackageManagerService.class,
                new Class<?>[]{Collection.class, boolean.class}, List.of(app, plain, platform, installer), false);
        set(pm, "mPackageEventSender", (java.util.function.Consumer<Intent>)event -> { });
        Object atm = make(type("ActivityTaskManagerService"), new Class<?>[]{PackageManagerService.class}, pm);
        Object am = make(type("ActivityManagerService"), new Class<?>[]{atm.getClass(), PackageManagerService.class,
                type("NotificationManagerService")}, atm, pm, null);
        if (!nativeFramework) {
            // Host only: the platform manifest's protection levels (signature plus flags).
            Object catalog = get(pm.permissions(), "mCatalog");
            Map<String, PermissionInfo> definitions = (Map<String, PermissionInfo>)get(catalog, "mFramework");
            for (String[] definition : DEFINITIONS) {
                PermissionInfo info = new PermissionInfo(); info.name = definition[0]; info.packageName = "android";
                info.protectionLevel = PermissionInfo.PROTECTION_SIGNATURE;
                for (String flag : definition[1].split("\\|")) {
                    info.protectionLevel |= switch (flag) {
                        case "appop" -> PermissionInfo.PROTECTION_FLAG_APPOP;
                        case "preinstalled" -> PermissionInfo.PROTECTION_FLAG_PREINSTALLED;
                        case "pre23" -> PermissionInfo.PROTECTION_FLAG_PRE23;
                        case "development" -> PermissionInfo.PROTECTION_FLAG_DEVELOPMENT;
                        case "role" -> PermissionInfo.PROTECTION_FLAG_ROLE;
                        case "privileged" -> PermissionInfo.PROTECTION_FLAG_PRIVILEGED;
                        default -> throw new AssertionError(flag);
                    };
                }
                definitions.put(info.name, info);
            }
            set(catalog, "mLoaded", true);
        }
        // The app is an attached process, so its Binder identity is authenticated as the app.
        as(APP_UID, APP_PID, () -> {
            IApplicationThread thread = new IApplicationThread.Default() {
                final Binder token = new Binder(); @Override public IBinder asBinder() { return token; }
            };
            Object process = make(type("ActivityTaskManagerService$ProcessRecord"),
                    new Class<?>[]{atm.getClass(), int.class, IApplicationThread.class}, atm, APP_PID, thread);
            set(process, "packageName", APP); set(process, "processName", APP);
            set(process, "packageCode", app); set(process, "ready", true);
            ((List<Object>)get(atm, "mProcesses")).add(process);
        });

        // A durable ledger, as the device keeps in data/system/appops.xml.
        File directory = new File(System.getProperty("java.io.tmpdir"), "gesher-special-access-" + System.nanoTime());
        check(directory.mkdirs(), "Cannot create the isolated AppOps directory");
        File file = new File(directory, "appops.xml");
        Supplier<Object> states = () -> {
            try { return call(am, "appOpsUidStates"); } catch (Exception e) { throw new AssertionError(e); }
        };
        AppOpsService ops = (AppOpsService)make(AppOpsService.class, new Class<?>[]{PackageManagerService.class, File.class}, pm, file);
        Method attach = AppOpsService.class.getDeclaredMethod("attach", type("CallerIdentity"), Supplier.class);
        attach.setAccessible(true);
        attach.invoke(ops, get(am, "mCallers"), states);
        AppOpsService transientOps = pm.appOps();
        set(pm, "mAppOps", ops);
        AppOpsService reopened = null;
        Object previousServices = field(type("Services"), "sLast").get(null);
        try {
            // Settings shows a switch only for what the manifest requests; all start off.
            List<String> initial = rows(pm, APP);
            check(initial.equals(List.of(
                    "android:manage_external_storage\tdenied", "android:system_alert_window\tdenied",
                    "android:request_install_packages\tdenied", "android:write_settings\tdenied",
                    "android:get_usage_stats\tdenied", "android:manage_media\tdenied")),
                    "Requested special accesses are listed, denied, in Settings' order: " + initial);
            check(rows(pm, PLAIN).isEmpty(), "An app requesting no special access has no switch");
            check(!appSees(pm, ops, AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE, Manifest.permission.MANAGE_EXTERNAL_STORAGE),
                    "isExternalStorageManager is false before the user allows it");
            check(!pm.canRequestPackageInstalls(APP, 0),
                    "Declaring REQUEST_INSTALL_PACKAGES alone does not allow install requests");
            check(ops.checkRawMode(AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE, APP_UID, APP)
                    == AppOpsManager.opToDefaultMode(AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE), "Default mode is AOSP's");

            // The app watches its op, as an OnOpChangedListener does.
            Watcher watcher = new Watcher();
            as(APP_UID, APP_PID, () -> ops.startWatchingMode(AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE, APP, watcher));

            // Grant: the app's own check reads it, without a fabricated permission grant.
            check(setAccess(pm, APP, FILES, true).equals(FILES + "\tallowed"), "Allowing answers the new state");
            check(ops.checkRawMode(AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE, APP_UID, APP) == AppOpsManager.MODE_ALLOWED,
                    "All files access is a MODE_ALLOWED UID mode");
            check(appSees(pm, ops, AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE, Manifest.permission.MANAGE_EXTERNAL_STORAGE),
                    "isExternalStorageManager reads the user's grant");
            check(pm.permissions().checkPermission(APP, Manifest.permission.MANAGE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_DENIED,
                    "The switch does not grant the signature permission itself");
            awaitCount(watcher.calls, 1);
            check(watcher.lastOp == AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE, "The listener hears its op");
            check(rows(pm, APP).get(0).equals(FILES + "\tallowed"), "The Control Center reads the grant back");
            check(rows(pm, APP).get(1).endsWith("\tdenied"), "One switch leaves the others off");

            // Revoke.
            check(setAccess(pm, APP, FILES, false).equals(FILES + "\tdenied"), "Denying answers the new state");
            check(ops.checkRawMode(AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE, APP_UID, APP) == AppOpsManager.MODE_ERRORED,
                    "Off is Settings' MODE_ERRORED");
            check(!appSees(pm, ops, AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE, Manifest.permission.MANAGE_EXTERNAL_STORAGE),
                    "isExternalStorageManager reads the revocation");
            awaitCount(watcher.calls, 2);
            check(setAccess(pm, APP, FILES, false).equals(FILES + "\tdenied"), "Denying again is idempotent");
            Thread.sleep(50);
            check(watcher.calls.get() == 2, "An unchanged mode notifies nobody");

            // Every other switch, with the mode Settings writes and the check the app makes.
            Object[][] others = {
                    {AppOpsManager.OPSTR_SYSTEM_ALERT_WINDOW, AppOpsManager.OP_SYSTEM_ALERT_WINDOW,
                            Manifest.permission.SYSTEM_ALERT_WINDOW, AppOpsManager.MODE_ERRORED},
                    {AppOpsManager.OPSTR_WRITE_SETTINGS, AppOpsManager.OP_WRITE_SETTINGS,
                            Manifest.permission.WRITE_SETTINGS, AppOpsManager.MODE_ERRORED},
                    {AppOpsManager.OPSTR_GET_USAGE_STATS, AppOpsManager.OP_GET_USAGE_STATS,
                            Manifest.permission.PACKAGE_USAGE_STATS, AppOpsManager.MODE_IGNORED},
                    {AppOpsManager.OPSTR_MANAGE_MEDIA, AppOpsManager.OP_MANAGE_MEDIA,
                            Manifest.permission.MANAGE_MEDIA, AppOpsManager.MODE_ERRORED},
                    {AppOpsManager.OPSTR_REQUEST_INSTALL_PACKAGES, AppOpsManager.OP_REQUEST_INSTALL_PACKAGES,
                            Manifest.permission.REQUEST_INSTALL_PACKAGES, AppOpsManager.MODE_ERRORED},
            };
            for (Object[] other : others) {
                String name = (String)other[0]; int op = (Integer)other[1]; String permission = (String)other[2];
                check(AppOpsManager.strOpToOp(name) == op, "The switch names its own op: " + name);
                check(!appSees(pm, ops, op, permission), name + " starts denied");
                check(setAccess(pm, APP, name, true).equals(name + "\tallowed"), name + " can be allowed");
                check(ops.checkRawMode(op, APP_UID, APP) == AppOpsManager.MODE_ALLOWED, name + " is MODE_ALLOWED");
                check(appSees(pm, ops, op, permission), name + " reads allowed in the app");
                check(pm.permissions().checkPermission(APP, permission) == PackageManager.PERMISSION_DENIED,
                        name + " does not grant the permission itself");
                check(setAccess(pm, APP, name, false).equals(name + "\tdenied"), name + " can be denied");
                check(ops.checkRawMode(op, APP_UID, APP) == (Integer)other[3], name + " off is Settings' denial mode");
                check(!appSees(pm, ops, op, permission), name + " reads denied in the app");
            }
            setAccess(pm, APP, AppOpsManager.OPSTR_REQUEST_INSTALL_PACKAGES, true);
            check(pm.canRequestPackageInstalls(APP, 0), "canRequestPackageInstalls follows Install unknown apps");
            check(!pm.canRequestPackageInstalls(APP, 10), "Only the one user");
            // The answer reveals a user decision: only the package's own caller may ask (AOSP).
            as(APP_UID, APP_PID, () -> check(pm.canRequestPackageInstalls(APP, 0), "An app may ask about itself"));
            as(APP_UID, APP_PID, () -> rejects(SecurityException.class, () -> pm.canRequestPackageInstalls(PLAIN, 0)));
            as(APP_UID, APP_PID, () -> rejects(SecurityException.class, () -> pm.canRequestPackageInstalls("fixture.absent", 0)));
            setAccess(pm, APP, AppOpsManager.OPSTR_REQUEST_INSTALL_PACKAGES, false);
            check(!pm.canRequestPackageInstalls(APP, 0), "Turning Install unknown apps off is read back");

            // Strict validation: undeclared, unknown and malformed input change nothing.
            rejects(IllegalArgumentException.class, () -> setAccess(pm, PLAIN, FILES, true));
            check(ops.checkRawMode(AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE, PLAIN_UID, PLAIN)
                    == AppOpsManager.opToDefaultMode(AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE),
                    "An app that does not request a special access cannot be given it");
            for (String op : new String[]{"android:camera", "android:schedule_exact_alarm", FILES + " ",
                    "ANDROID:MANAGE_EXTERNAL_STORAGE", "", null}) {
                rejects(IllegalArgumentException.class, () -> setAccess(pm, APP, op, true));
            }
            for (String name : new String[]{"fixture.absent", "../fixture.access.app", "fixture access", "fixture",
                    "", null, "fixture.access.app\tx"}) {
                rejects(IllegalArgumentException.class, () -> setAccess(pm, name, FILES, true));
                rejects(IllegalArgumentException.class, () -> rows(pm, name));
            }
            check(ops.checkRawMode(AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE, APP_UID, APP) == AppOpsManager.MODE_ERRORED,
                    "Rejected requests left the app's decision alone");

            // An attached app cannot reach the write: not through AppOps, not through the
            // special access path, not through the gesher service.
            as(APP_UID, APP_PID, () -> rejects(SecurityException.class,
                    () -> ops.setUidMode(AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE, APP_UID, AppOpsManager.MODE_ALLOWED)));
            as(APP_UID, APP_PID, () -> rejects(SecurityException.class,
                    () -> ops.setMode(AppOpsManager.OP_SYSTEM_ALERT_WINDOW, APP_UID, APP, AppOpsManager.MODE_ALLOWED)));
            as(APP_UID, APP_PID, () -> rejects(SecurityException.class, () -> setAccess(pm, APP, FILES, true)));
            Object created = make(type("Services$Created"), new Class<?>[]{PackageManagerService.class, atm.getClass(),
                    am.getClass(), type("ClipboardService"), Map.class}, pm, atm, am, null, new java.util.HashMap<>());
            field(type("Services"), "sLast").set(null, created);
            Class<?> deviceType = type("GesherService$Device");
            Object device = Proxy.newProxyInstance(deviceType.getClassLoader(), new Class<?>[]{deviceType}, (proxy, method, args) -> {
                switch (method.getName()) {
                    case "specialAccess": return invoke(type("SpecialAccess"), "rows", invoke(type("SpecialAccess"), "query", pm, args[0]));
                    case "setSpecialAccess": return setAccess(pm, (String)args[0], (String)args[1], (Boolean)args[2]);
                    case "settingsScreensClosed": return ((String[])args[0]).length;
                    case "touch": return null;
                    default: throw new AssertionError("Unexpected device call " + method.getName());
                }
            });
            IBinder gesher = (IBinder)make(type("GesherService"), new Class<?>[]{deviceType}, device);
            int setCode = (Integer)field(type("GesherService"), "SET_SPECIAL_ACCESS").get(null);
            int queryCode = (Integer)field(type("GesherService"), "SPECIAL_ACCESS").get(null);
            String descriptor = (String)field(type("GesherService"), "DESCRIPTOR").get(null);
            as(APP_UID, APP_PID, () -> rejects(SecurityException.class, () -> {
                Parcel data = Parcel.obtain(), reply = Parcel.obtain();
                try {
                    data.writeInterfaceToken(descriptor); data.writeString(APP); data.writeString(FILES); data.writeBoolean(true);
                    gesher.transact(setCode, data, reply, 0);
                    reply.readException();
                } finally { data.recycle(); reply.recycle(); }
            }));
            as(APP_UID, APP_PID, () -> rejects(SecurityException.class, () -> {
                Parcel data = Parcel.obtain(), reply = Parcel.obtain();
                try {
                    data.writeInterfaceToken(descriptor); data.writeString(APP);
                    gesher.transact(queryCode, data, reply, 0);
                    reply.readException();
                } finally { data.recycle(); reply.recycle(); }
            }));
            check(!appSees(pm, ops, AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE, Manifest.permission.MANAGE_EXTERNAL_STORAGE),
                    "The app could not grant itself all files access");

            // The host shell's transaction (this process is the system here) is the Control Center's path.
            Parcel data = Parcel.obtain(), reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(descriptor); data.writeString(APP); data.writeString(FILES); data.writeBoolean(true);
                check(gesher.transact(setCode, data, reply, 0), "The gesher service handles the switch");
                reply.readException();
                check((FILES + "\tallowed").equals(reply.readString()), "The transaction answers the new state");
            } finally { data.recycle(); reply.recycle(); }
            data = Parcel.obtain(); reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(descriptor); data.writeString(APP); data.writeString("android:camera"); data.writeBoolean(true);
                gesher.transact(setCode, data, reply, 0);
                rejects(IllegalArgumentException.class, reply::readException);
            } finally { data.recycle(); reply.recycle(); }
            data = Parcel.obtain(); reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(descriptor); data.writeString(APP);
                gesher.transact(queryCode, data, reply, 0);
                reply.readException();
                String[] answered = reply.createStringArray();
                check(answered.length == 6 && answered[0].equals(FILES + "\tallowed"), "The query transaction lists the switches");
            } finally { data.recycle(); reply.recycle(); }
            check(appSees(pm, ops, AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE, Manifest.permission.MANAGE_EXTERNAL_STORAGE),
                    "The host's grant is what the app reads");
            awaitCount(watcher.calls, 3);

            // The launcher's client: rows, refusals and an older device without the codes.
            Constructor<?> clientConstructor = type("GesherService$Client").getDeclaredConstructor(IBinder.class);
            clientConstructor.setAccessible(true);
            Object client = clientConstructor.newInstance(gesher);
            String[] clientRows = (String[])clientCall(client, "specialAccess", APP);
            check(clientRows.length == 6 && clientRows[0].equals(FILES + "\tallowed"), "Client reads the switches");
            check((FILES + "\tdenied").equals(clientCall(client, "setSpecialAccess", APP, FILES, false)), "Client changes a switch");
            check((FILES + "\tallowed").equals(clientCall(client, "setSpecialAccess", APP, FILES, true)), "Client changes it back");
            rejects(IllegalArgumentException.class, () -> clientCall(client, "setSpecialAccess", APP, "android:camera", true));
            check((Integer)clientCall(client, "settingsScreensClosed", (Object)new String[]{"0123456789abcdef0123456789abcdef"}) == 1,
                    "Client reports closed Settings requests");
            rejects(IllegalArgumentException.class, () -> clientCall(client, "settingsScreensClosed", (Object)new String[]{"../x"}));
            Object older = clientConstructor.newInstance(new Binder());
            rejects(UnsupportedOperationException.class, () -> clientCall(older, "specialAccess", APP));
            rejects(UnsupportedOperationException.class, () -> clientCall(older, "setSpecialAccess", APP, FILES, true));
            rejects(UnsupportedOperationException.class, () -> clientCall(older, "settingsScreensClosed", (Object)new String[0]));
            // Launcher special-access: exit codes and the protocol rows on standard output.
            Method launcher = Class.forName("com.gesher.app.Launcher").getDeclaredMethod("specialAccess",
                    type("GesherService$Client"), List.class, java.io.PrintStream.class);
            launcher.setAccessible(true);
            java.io.ByteArrayOutputStream printed = new java.io.ByteArrayOutputStream();
            java.io.PrintStream out = new java.io.PrintStream(printed, true, "UTF-8");
            check((Integer)launcher.invoke(null, client, List.of(APP), out) == 0, "special-access exits 0");
            String text = printed.toString("UTF-8");
            check(text.startsWith("special-access\t" + FILES + "\tallowed") && text.split("\n").length == 6,
                    "special-access prints one row per switch: " + text);
            printed.reset();
            check((Integer)launcher.invoke(null, client, List.of(APP, "android:write_settings", "allow"), out) == 0
                    && printed.toString("UTF-8").trim().equals("special-access\tandroid:write_settings\tallowed"),
                    "special-access OP allow prints the new state");
            setAccess(pm, APP, "android:write_settings", false);
            printed.reset();
            check((Integer)launcher.invoke(null, client, List.of(APP, "android:camera", "allow"), out) == 1
                    && printed.size() == 0, "A refused change exits 1 with no row");
            check((Integer)launcher.invoke(null, client, List.of("fixture.absent"), out) == 1, "An unknown package exits 1");
            check((Integer)launcher.invoke(null, older, List.of(APP), out) == 4 && printed.size() == 0,
                    "An older device exits 4 with no row");

            // A stale package generation is refused before anything is written (package mode).
            ApkPackage replaced = pkg(APP, APP_UID, (byte)17, requested);
            int overlay = ops.checkRawMode(AppOpsManager.OP_SYSTEM_ALERT_WINDOW, APP_UID, APP);
            check(!ops.setModeIfCurrent(AppOpsManager.OP_SYSTEM_ALERT_WINDOW, replaced,
                    overlay == AppOpsManager.MODE_ALLOWED ? AppOpsManager.MODE_ERRORED : AppOpsManager.MODE_ALLOWED),
                    "An obsolete package generation cannot write a package mode");
            check(ops.checkRawMode(AppOpsManager.OP_SYSTEM_ALERT_WINDOW, APP_UID, APP) == overlay,
                    "The refused write left the mode alone");
            as(APP_UID, APP_PID, () -> rejects(SecurityException.class,
                    () -> ops.setModeIfCurrent(AppOpsManager.OP_SYSTEM_ALERT_WINDOW, app, AppOpsManager.MODE_ALLOWED)));

            // An INSTALL_PACKAGES installer: allowed whatever the mode, so the switch is fixed.
            check(pm.permissions().checkPermission(INSTALLER, Manifest.permission.INSTALL_PACKAGES) == PackageManager.PERMISSION_GRANTED,
                    "The platform-signed fixture holds INSTALL_PACKAGES");
            check(rows(pm, INSTALLER).equals(List.of("android:request_install_packages\tallowed\tfixed")),
                    "Install unknown apps is shown fixed on for an installer: " + rows(pm, INSTALLER));
            rejects(IllegalStateException.class, () -> setAccess(pm, INSTALLER, AppOpsManager.OPSTR_REQUEST_INSTALL_PACKAGES, false));
            check(pm.canRequestPackageInstalls(INSTALLER, 0) && ops.checkRawMode(AppOpsManager.OP_REQUEST_INSTALL_PACKAGES,
                    INSTALLER_UID, INSTALLER) == AppOpsManager.opToDefaultMode(AppOpsManager.OP_REQUEST_INSTALL_PACKAGES),
                    "The refused change wrote nothing and the installer may still install");

            // Storage stats follow the Usage access op (AOSP checkStatsPermission), not the manifest.
            File statsData = new File(directory, "data");
            check(new File(statsData, "media/0").mkdirs(), "Cannot create the storage stats data directory");
            Object stats = make(type("StorageStatsService"), new Class<?>[]{PackageManagerService.class, File.class}, pm, statsData);
            Method forUser = stats.getClass().getMethod("queryExternalStatsForUser", String.class, int.class, String.class);
            Checked queryAsApp = () -> as(APP_UID, APP_PID, () -> forUser.invoke(stats, null, 0, APP));
            setAccess(pm, APP, AppOpsManager.OPSTR_GET_USAGE_STATS, false);
            rejects(SecurityException.class, queryAsApp);
            ops.setMode(AppOpsManager.OP_GET_USAGE_STATS, APP_UID, APP, AppOpsManager.MODE_DEFAULT);
            rejects(SecurityException.class, queryAsApp);
            setAccess(pm, APP, AppOpsManager.OPSTR_GET_USAGE_STATS, true);
            queryAsApp.run();
            checks++;
            as(APP_UID, APP_PID, () -> rejects(SecurityException.class, () -> forUser.invoke(stats, null, 0, PLAIN)));
            setAccess(pm, APP, AppOpsManager.OPSTR_GET_USAGE_STATS, false);
            rejects(SecurityException.class, queryAsApp);
            Method forUid = stats.getClass().getMethod("getCacheQuotaBytes", String.class, int.class, String.class);
            // Its own uid needs no usage access.
            as(APP_UID, APP_PID, () -> forUid.invoke(stats, null, APP_UID, APP));
            as(APP_UID, APP_PID, () -> rejects(SecurityException.class, () -> forUid.invoke(stats, null, PLAIN_UID, APP)));
            checks++;

            // A per-app Settings screen opened for a result is answered when the Control
            // Center reports that its details closed (SettingsResults through the real
            // activity starter), not at once; RESULT_OK for Install unknown apps once allowed.
            Object process = ((List<Object>)get(atm, "mProcesses")).get(0);
            IApplicationThread appThread = (IApplicationThread)get(process, "thread");
            // Another attached app, to try a result token that is not its own.
            IApplicationThread plainThread = new IApplicationThread.Default() {
                final Binder token = new Binder(); @Override public IBinder asBinder() { return token; }
            };
            as(PLAIN_UID, APP_PID + 50, () -> {
                Object plainProcess = make(type("ActivityTaskManagerService$ProcessRecord"),
                        new Class<?>[]{atm.getClass(), int.class, IApplicationThread.class}, atm, APP_PID + 50, plainThread);
                set(plainProcess, "packageName", PLAIN); set(plainProcess, "processName", PLAIN);
                set(plainProcess, "packageCode", plain); set(plainProcess, "ready", true);
                ((List<Object>)get(atm, "mProcesses")).add(plainProcess);
            });
            android.content.pm.ActivityInfo activityInfo = new android.content.pm.ActivityInfo();
            activityInfo.packageName = APP; activityInfo.name = APP + ".Store";
            Class<?> recordType = type("ActivityTaskManagerService$ActivityRecord");
            Object caller = make(recordType, new Class<?>[]{atm.getClass(), android.content.pm.ActivityInfo.class, Intent.class,
                    int.class, int.class, String.class, recordType, String.class, int.class},
                    atm, activityInfo, new Intent(), 1, APP_PID, APP, null, null, 0);
            set(caller, "app", process);
            Class stateType = type("ActivityTaskManagerService$State");
            set(caller, "state", Enum.valueOf(stateType, "PAUSED"));  // results queue on the record
            ((List<Object>)get(atm, "mActivities")).add(caller);
            IBinder callerToken = (IBinder)get(caller, "token");
            List<Object> results = (List<Object>)get(caller, "pendingResults");
            Method starter = null;
            for (Method m : atm.getClass().getDeclaredMethods()) {
                if (m.getName().equals("start") && m.getParameterCount() == 8) starter = m;
            }
            starter.setAccessible(true);
            final Method start = starter;
            Field forResult = type("ControlCenter").getDeclaredField("sOpenForResult");
            forResult.setAccessible(true);
            Object previousForResult = forResult.get(null);
            Field openPlain = type("ControlCenter").getDeclaredField("sOpen");
            openPlain.setAccessible(true);
            Object previousOpen = openPlain.get(null);
            List<String> plainOpens = new java.util.ArrayList<>();
            openPlain.set(null, (java.util.function.Predicate<String>)pkg -> plainOpens.add(String.valueOf(pkg)));
            List<String> requests = new java.util.ArrayList<>();
            java.util.function.Consumer<String>[] lastReplaced = new java.util.function.Consumer[1];
            forResult.set(null, opener((pkg, token, told) -> { lastReplaced[0] = told; return requests.add(pkg + "\t" + token); }));
            Method toggled = atm.getClass().getMethod("specialAccessToggled", String.class, String.class, boolean.class);
            Intent unknownSources = new Intent("android.settings.MANAGE_UNKNOWN_APP_SOURCES", android.net.Uri.parse("package:" + APP));
            Object[] status = new Object[1];
            try {
                setAccess(pm, APP, AppOpsManager.OPSTR_REQUEST_INSTALL_PACKAGES, false);
                as(APP_UID, APP_PID, () -> status[0] = start.invoke(atm, appThread, unknownSources, null, APP, callerToken, "who", 7, null));
                check(Integer.valueOf(android.app.ActivityManager.START_SUCCESS).equals(status[0]), "The screen opened: " + status[0]);
                check(requests.size() == 1 && requests.get(0).matches(java.util.regex.Pattern.quote(APP) + "\t[0-9a-f]{32}"),
                        "The Control Center request carries a result token: " + requests);
                check(results.isEmpty(), "No result before the user decided");
                String token = requests.get(0).substring(APP.length() + 1);
                // The user turns Install unknown apps on, then closes the details (the device
                // tells the waiting screens of the turn, as PersistentDevice does).
                setAccess(pm, APP, AppOpsManager.OPSTR_REQUEST_INSTALL_PACKAGES, true);
                toggled.invoke(atm, APP, AppOpsManager.OPSTR_REQUEST_INSTALL_PACKAGES, true);
                check(atm.getClass().getMethod("settingsScreensClosed", String[].class).invoke(atm, (Object)new String[]{token}).equals(1),
                        "The close report is accepted");
                check(results.size() == 1 && (Integer)get(results.get(0), "mResultCode") == android.app.Activity.RESULT_OK
                        && (Integer)get(results.get(0), "mRequestCode") == 7, "Install unknown apps turned on answers RESULT_OK");
                check(atm.getClass().getMethod("settingsScreensClosed", String[].class).invoke(atm, (Object)new String[]{token}).equals(0)
                        && results.size() == 1, "A result is delivered once");
                // Left off, or another screen: RESULT_CANCELED, still after the close.
                setAccess(pm, APP, AppOpsManager.OPSTR_REQUEST_INSTALL_PACKAGES, false);
                as(APP_UID, APP_PID, () -> start.invoke(atm, appThread, unknownSources, null, APP, callerToken, "who", 8, null));
                check(results.size() == 1, "The second request waits too");
                atm.getClass().getMethod("settingsScreensClosed", String[].class).invoke(atm, (Object)new String[]{requests.get(1).substring(APP.length() + 1)});
                check(results.size() == 2 && (Integer)get(results.get(1), "mResultCode") == android.app.Activity.RESULT_CANCELED,
                        "Install unknown apps left off answers RESULT_CANCELED");
                // The caller finished meanwhile: nothing is delivered, and nothing waits.
                as(APP_UID, APP_PID, () -> start.invoke(atm, appThread, unknownSources, null, APP, callerToken, "who", 9, null));
                set(caller, "finishing", true);
                atm.getClass().getMethod("settingsScreensClosed", String[].class).invoke(atm, (Object)new String[]{requests.get(2).substring(APP.length() + 1)});
                check(results.size() == 2, "A finishing caller gets no result");
                set(caller, "finishing", false);
                Object pendingResults = get(atm, "mSettingsResults");
                check((Integer)call(pendingResults, "pendingCount") == 0, "Nothing is left waiting");
                // Already allowed and closed without a turn: RESULT_CANCELED, as AOSP.
                setAccess(pm, APP, AppOpsManager.OPSTR_REQUEST_INSTALL_PACKAGES, true);
                as(APP_UID, APP_PID, () -> start.invoke(atm, appThread, unknownSources, null, APP, callerToken, "who", 12, null));
                atm.getClass().getMethod("settingsScreensClosed", String[].class).invoke(atm, (Object)new String[]{requests.get(3).substring(APP.length() + 1)});
                check(results.size() == 3 && (Integer)get(results.get(2), "mResultCode") == android.app.Activity.RESULT_CANCELED
                        && (Integer)get(results.get(2), "mRequestCode") == 12, "An app already allowed, closed without a turn, gets RESULT_CANCELED");
                // A waiting request that another app's request replaced before it was shown is
                // answered at once; the newer one keeps waiting.
                as(APP_UID, APP_PID, () -> start.invoke(atm, appThread, unknownSources, null, APP, callerToken, "who", 13, null));
                String replacedToken = requests.get(4).substring(APP.length() + 1);
                as(APP_UID, APP_PID, () -> start.invoke(atm, appThread, unknownSources, null, APP, callerToken, "who", 14, null));
                check(results.size() == 3, "Both requests wait");
                lastReplaced[0].accept(replacedToken);
                check(results.size() == 4 && (Integer)get(results.get(3), "mRequestCode") == 13
                        && (Integer)get(results.get(3), "mResultCode") == android.app.Activity.RESULT_CANCELED,
                        "A replaced request is answered at once, not after the timeout");
                atm.getClass().getMethod("settingsScreensClosed", String[].class).invoke(atm, (Object)new String[]{requests.get(5).substring(APP.length() + 1)});
                check(results.size() == 5 && (Integer)get(results.get(4), "mRequestCode") == 14, "The newer request is answered on its close");
                setAccess(pm, APP, AppOpsManager.OPSTR_REQUEST_INSTALL_PACKAGES, false);
                // A Control Center that does not open: no token kept, the start fails.
                forResult.set(null, opener((pkg, token2, told) -> false));
                as(APP_UID, APP_PID, () -> status[0] = start.invoke(atm, appThread, unknownSources, null, APP, callerToken, "who", 10, null));
                check(Integer.valueOf(android.app.ActivityManager.START_INTENT_NOT_RESOLVED).equals(status[0])
                        && (Integer)call(pendingResults, "pendingCount") == 0, "A request the Control Center never saw is not kept");
                // Another process's token gets no result at all (the calling-process guard).
                forResult.set(null, opener((pkg, token3, told) -> requests.add(pkg + "\t" + token3)));
                int before = requests.size();
                as(PLAIN_UID, APP_PID + 50, () -> start.invoke(atm, plainThread, unknownSources, null, PLAIN, callerToken, "who", 11, null));
                check(requests.size() == before && plainOpens.equals(List.of(APP))
                        && (Integer)call(pendingResults, "pendingCount") == 0 && results.size() == 5,
                        "A foreign result token neither waits nor receives a result");
            } finally {
                forResult.set(null, previousForResult);
                openPlain.set(null, previousOpen);
                ((List<Object>)get(atm, "mActivities")).remove(caller);
            }

            // The request files: one token file per waiting request; a request for another
            // app replaces the unshown ones (told), one for the same app keeps them.
            File requestFile = new File(directory, "system/control-panel-request");
            File resultsDir = new File(directory, "system/control-panel-results");
            Method publish = type("ControlCenter").getDeclaredMethod("publishRequest", File.class, File.class,
                    String.class, String.class, java.util.function.Consumer.class);
            publish.setAccessible(true);
            List<String> told = new java.util.ArrayList<>();
            java.util.function.Consumer<String> tell = told::add;
            String ta = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", tb = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", tc = "cccccccccccccccccccccccccccccccc";
            publish.invoke(null, requestFile, resultsDir, "com.example.first", ta, tell);
            check(new String(java.nio.file.Files.readAllBytes(requestFile.toPath()), "UTF-8").equals("com.example.first\n")
                    && new File(resultsDir, ta).isFile() && told.isEmpty(), "The request file names the package; its token has a file");
            publish.invoke(null, requestFile, resultsDir, "com.example.first", tb, tell);
            check(new File(resultsDir, ta).isFile() && new File(resultsDir, tb).isFile() && told.isEmpty(),
                    "A second request for the same app keeps the first token");
            publish.invoke(null, requestFile, resultsDir, "com.example.second", tc, tell);
            check(!new File(resultsDir, ta).exists() && !new File(resultsDir, tb).exists() && new File(resultsDir, tc).isFile()
                    && told.size() == 2 && told.containsAll(List.of(ta, tb)), "Another app's request replaces and tells the unshown ones: " + told);
            publish.invoke(null, requestFile, resultsDir, "com.example.third", null, tell);
            check(!new File(resultsDir, tc).exists() && told.size() == 3 && told.get(2).equals(tc)
                    && new String(java.nio.file.Files.readAllBytes(requestFile.toPath()), "UTF-8").equals("com.example.third\n"),
                    "A request with no token replaces waiting ones too");
            rejects(java.io.IOException.class, () -> publish.invoke(null, requestFile, resultsDir, "com.example.x", "../evil", tell));

            // Persistence: a restarted ledger over the same file has the user's decisions.
            setAccess(pm, APP, AppOpsManager.OPSTR_SYSTEM_ALERT_WINDOW, true);
            ops.reloadNonHistoricalState();
            reopened = (AppOpsService)make(AppOpsService.class, new Class<?>[]{PackageManagerService.class, File.class}, pm, file);
            attach.invoke(reopened, get(am, "mCallers"), states);
            check(reopened.checkRawMode(AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE, APP_UID, APP) == AppOpsManager.MODE_ALLOWED,
                    "All files access survives a restart");
            check(reopened.checkRawMode(AppOpsManager.OP_SYSTEM_ALERT_WINDOW, APP_UID, APP) == AppOpsManager.MODE_ALLOWED,
                    "Display over other apps survives a restart");
            check(reopened.checkRawMode(AppOpsManager.OP_WRITE_SETTINGS, APP_UID, APP) == AppOpsManager.MODE_ERRORED,
                    "A denial survives a restart");
            set(pm, "mAppOps", reopened);
            List<String> afterRestart = rows(pm, APP);
            check(afterRestart.get(0).equals(FILES + "\tallowed") && afterRestart.get(1).endsWith("\tallowed")
                    && afterRestart.get(3).endsWith("\tdenied"), "The Control Center reads the restored state: " + afterRestart);
            as(APP_UID, APP_PID, () -> ops.stopWatchingMode(watcher));
        } finally {
            field(type("Services"), "sLast").set(null, previousServices);
            set(pm, "mAppOps", transientOps);
            for (AppOpsService service : new AppOpsService[]{ops, reopened, transientOps}) {
                if (service == null) continue;
                ScheduledThreadPoolExecutor events = (ScheduledThreadPoolExecutor)get(service, "mEvents");
                events.shutdownNow();
                check(events.awaitTermination(5, TimeUnit.SECONDS), "Owned AppOps callbacks did not retire");
            }
            File[] leftovers = directory.listFiles();
            if (leftovers != null) for (File leftover : leftovers) leftover.delete();
            directory.delete();
        }
        return checks;
    }

    private interface OpenCall { boolean open(String pkg, String token, java.util.function.Consumer<String> replaced); }
    @SuppressWarnings("unchecked")
    private static Object opener(OpenCall call) throws Exception {
        Class<?> type = type("ControlCenter$Opener");
        return Proxy.newProxyInstance(type.getClassLoader(), new Class<?>[]{type},
                (proxy, method, args) -> call.open((String)args[0], (String)args[1], (java.util.function.Consumer<String>)args[2]));
    }

    private static Object clientCall(Object client, String name, Object... args) throws Exception {
        for (Method method : client.getClass().getDeclaredMethods()) {
            if (method.getName().equals(name) && method.getParameterCount() == args.length) {
                method.setAccessible(true);
                try { return method.invoke(client, args); }
                catch (InvocationTargetException e) {
                    if (e.getCause() instanceof Exception cause) throw cause;
                    throw e;
                }
            }
        }
        throw new NoSuchMethodException(name);
    }

    private static Object call(Object target, String name) throws Exception {
        Method method = target.getClass().getDeclaredMethod(name); method.setAccessible(true);
        return method.invoke(target);
    }

    public static void main(String[] args) throws Exception {
        int total = run(true);
        System.out.println("SpecialAccessTest all passed: " + total + " checks");
    }
}
