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
        PackageManagerService pm = (PackageManagerService)make(PackageManagerService.class,
                new Class<?>[]{Collection.class, boolean.class}, List.of(app, plain), false);
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

    private static Object call(Object target, String name) throws Exception {
        Method method = target.getClass().getDeclaredMethod(name); method.setAccessible(true);
        return method.invoke(target);
    }

    public static void main(String[] args) throws Exception {
        int total = run(true);
        System.out.println("SpecialAccessTest all passed: " + total + " checks");
    }
}
