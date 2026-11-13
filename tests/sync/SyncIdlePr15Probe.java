/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.accounts.Account;
import android.content.SyncResult;
import android.os.Bundle;
import android.os.SystemClock;
import com.gesher.server.DeviceWork;
import gesher.services.SyncManagerContractTest;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

/** Explicit host-only probe of a fully built candidate plus PR15 application. */
public final class SyncIdlePr15Probe {
    private static int checks;
    private static Method keepsIdleDevice;

    private static void check(boolean ok, String message) {
        if (!ok) throw new AssertionError(message);
        checks++;
    }

    private static Object field(Object target, String name) throws Exception {
        Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(target);
    }

    private static void set(Object target, String name, Object value) throws Exception {
        Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(target, value);
    }

    private static Object call(Object target, String name, Class<?>[] types,
            Object... args) throws Exception {
        Method method = target.getClass().getDeclaredMethod(name, types);
        method.setAccessible(true);
        try { return method.invoke(target, args); }
        catch (InvocationTargetException failure) { throw new AssertionError(failure.getCause()); }
    }

    private static boolean keeps(long idle, long timeout) throws Exception {
        return (Boolean) keepsIdleDevice.invoke(null, idle, timeout);
    }

    private static void requireCombinedApp() throws Exception {
        try {
            keepsIdleDevice = DeviceWork.class.getMethod("keepsIdleDevice", long.class, long.class);
            Class<?> process = Class.forName("com.gesher.server.ActivityTaskManagerService$ProcessRecord");
            Class.forName("com.gesher.server.ActivityManagerService")
                    .getDeclaredMethod("keepsDeviceBusy", process);
        } catch (NoSuchMethodException missing) {
            throw new IllegalStateException("--idle-policy-probe requires --app-classes from the "
                    + "full application built with both this change and PR15's idle policy; "
                    + "required production idle-policy methods are absent", missing);
        }
    }

    @SuppressWarnings("unchecked")
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        requireCombinedApp();
        SyncManagerContractTest.contracts("all", false);
        int ordinaryChecks = SyncManagerContractTest.checks;
        check(!keeps(600000, 600000), "Ordinary sync fixtures retained device idle protection");

        Class<?> harness = Class.forName("gesher.services.SyncManagerContractTest$Harness");
        Constructor<?> constructor = harness.getDeclaredConstructor(boolean.class, boolean.class);
        constructor.setAccessible(true);
        Object h = constructor.newInstance(false, false);
        try {
            Object timer = field(h, "timer"), am = field(h, "am"), atm = field(h, "atm");
            // DeviceWork reads the host elapsed clock. Use that same time domain for
            // the manually dispatched SyncManager's future retry deadlines.
            set(timer, "now", SystemClock.elapsedRealtime());
            Account account = new Account("one", "gesher.sync.account");
            Class<?>[] request = {Account.class, Bundle.class};
            call(h, "enqueue", request, account, new Bundle());
            call(timer, "drain", new Class<?>[0]);
            check((int) call(h, "active", new Class<?>[0]) == 1
                    && (int) call(h, "bindings", new Class<?>[0]) == 1,
                    "Real sync must be admitted and own its service binding");
            List<Object> processes = (List<Object>) field(atm, "mProcesses");
            Object process = processes.get(0);
            List<Object> activities = (List<Object>) field(process, "activities");
            List<Object> saved = new ArrayList<>(activities);
            activities.clear();
            try {
                check(!(boolean) call(am, "keepsDeviceBusy", new Class<?>[]{process.getClass()}, process),
                        "PR15 must classify the background sync service as background work");
                check((long) call(atm, "idleSince", new Class<?>[0]) != 0,
                        "PR15 idle clock must advance after activity closes despite system binding");
                check(keeps(6000000, 600000),
                        "Active sync must prevent PR15 shutdown after ten idle timeouts");
            } finally {
                // Retain the fixture's existing process-cleanup boundary after the
                // idle-policy observation; this probe does not terminate a live app.
                activities.addAll(saved);
            }
            call(h, "finish", new Class<?>[]{int.class, SyncResult.class}, 0, new SyncResult());
            check((int) call(h, "active", new Class<?>[0]) == 0
                    && (int) call(h, "bindings", new Class<?>[0]) == 0,
                    "Completed sync must release registry and binding");
            check(!keeps(6000000, 600000), "Retired sync must release PR15 idle shutdown");

            call(h, "enqueue", request, account, new Bundle());
            call(timer, "drain", new Class<?>[0]);
            SyncResult soft = new SyncResult();
            soft.stats.numIoExceptions = 1;
            set(timer, "now", SystemClock.elapsedRealtime());
            call(h, "finish", new Class<?>[]{int.class, SyncResult.class}, 1, soft);
            check((int) call(h, "pending", new Class<?>[0]) == 1, "Soft error must create pending retry");
            check(keeps(600000, 600000), "Future retry must keep first PR15 idle grace interval");
            check(!keeps(1200000, 600000), "Future retry must release PR15 idle after two timeouts");
            call(h, "cancel", new Class<?>[]{Account.class, String.class}, account, "gesher.sync.rows");
            call(timer, "drain", new Class<?>[0]);
            check(!keeps(600000, 600000), "Cancelled future retry must release first idle grace interval");
        } finally {
            ((AutoCloseable) h).close();
        }
        System.out.println("Sync manager host passed: " + ordinaryChecks + " checks");
        System.out.println("Sync/PR15 coexistence passed: " + checks
                + " additional idle-policy checks; " + (SyncManagerContractTest.checks - ordinaryChecks)
                + " supporting fixture checks; full merged production app, real service registry and DeviceWork idle policy");
    }
}
