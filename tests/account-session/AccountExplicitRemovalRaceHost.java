/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.accounts.AccountManager;
import com.sun.jdi.Bootstrap;
import com.sun.jdi.ClassType;
import com.sun.jdi.Location;
import com.sun.jdi.Method;
import com.sun.jdi.ObjectReference;
import com.sun.jdi.VMDisconnectedException;
import com.sun.jdi.VirtualMachine;
import com.sun.jdi.connect.Connector;
import com.sun.jdi.connect.LaunchingConnector;
import com.sun.jdi.event.BreakpointEvent;
import com.sun.jdi.event.ClassPrepareEvent;
import com.sun.jdi.event.Event;
import com.sun.jdi.event.EventSet;
import com.sun.jdi.event.VMDeathEvent;
import com.sun.jdi.event.VMDisconnectEvent;
import com.sun.jdi.request.BreakpointRequest;
import com.sun.jdi.request.ClassPrepareRequest;
import java.io.IOException;
import java.util.Comparator;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.concurrent.TimeUnit;

/** JDI controls only scheduling after mutation and monitor exit, before notification.
 * The production AccountManagerService bytecode and dispatch path stay unchanged.
 */
public final class AccountExplicitRemovalRaceHost {
    private static AccountRetirementHost.H current;
    private static String transition;

    /** Called on the paused service thread after it has released the account monitor. */
    public static void transition() throws Exception {
        var h = current;
        AccountRetirementHost.check(!Thread.holdsLock(h.accounts), "Boundary still owns account monitor");
        AccountRetirementHost.check(!h.entries.containsKey(AccountRetirementHost.REMOVED),
                "Boundary precedes successful removal");
        if (transition.equals("unchanged")) return;
        if (transition.equals("reinstall")) {
            AccountRetirementHost.call(h.pm, "beginPackageRemoval",
                    new Class<?>[]{AccountRetirementHost.t("ApkPackage")}, h.client);
            AccountRetirementHost.call(h.accounts, "packageRemoved",
                    new Class<?>[]{String.class}, AccountRetirementHost.CLIENT);
            AccountRetirementHost.call(h.pm, "remove",
                    new Class<?>[]{String.class}, AccountRetirementHost.CLIENT);
            AccountRetirementHost.call(h.pm, "endPackageRemoval",
                    new Class<?>[]{AccountRetirementHost.t("ApkPackage")}, h.client);
        }
        AccountRetirementHost.call(h.pm, "add", new Class<?>[]{AccountRetirementHost.t("ApkPackage")},
                AccountRetirementHost.pkg(AccountRetirementHost.CLIENT, 12002));
        ((List<?>)AccountRetirementHost.get(h.atm, "mProcesses")).remove(h.process);
        h.connectReceiver();
    }

    private static void target(String nativeLibrary, String mode) throws Exception {
        System.load(nativeLibrary);
        android.app.PropertyInvalidatedCache.setTestMode(true);
        transition = mode;
        try (var h = new AccountRetirementHost.H()) {
            current = h;
            h.connectReceiver();
            AccountRetirementHost.check((Boolean)AccountRetirementHost.call(h.accounts,
                    "removeAccountExplicitly", new Class<?>[]{android.accounts.Account.class},
                    AccountRetirementHost.REMOVED), "Explicit removal did not succeed");
            var removed = h.thread.events.stream()
                    .filter(i -> AccountManager.ACTION_ACCOUNT_REMOVED.equals(i.getAction())).toList();
            AccountRetirementHost.check(removed.size() == (mode.equals("reinstall") ? 0 : 1),
                    "Removal event crossed installation lifetime: " + mode);
            if (!removed.isEmpty()) {
                AccountRetirementHost.check(AccountRetirementHost.REMOVED.name.equals(removed.get(0)
                        .getStringExtra(AccountManager.KEY_ACCOUNT_NAME)), "Removal name changed");
                AccountRetirementHost.check(AccountRetirementHost.REMOVED.type.equals(removed.get(0)
                        .getStringExtra(AccountManager.KEY_ACCOUNT_TYPE)), "Removal type changed");
            }
            AccountRetirementHost.check(!h.entries.containsKey(AccountRetirementHost.REMOVED)
                    && h.entries.containsKey(AccountRetirementHost.RETAINED), "Removal escaped its account scope");
            System.out.println("Explicit removal " + mode + " passed: " + AccountRetirementHost.checks + " checks");
        }
    }

    private static void failedWrite() throws Exception {
        try (var h = new AccountRetirementHost.H()) {
            h.connectReceiver();
            Properties before = h.durable;
            h.fail = true;
            try {
                AccountRetirementHost.call(h.accounts, "removeAccountExplicitly",
                        new Class<?>[]{android.accounts.Account.class}, AccountRetirementHost.REMOVED);
                throw new AssertionError("Failed account persistence reported success");
            } catch (IllegalStateException expected) {
                AccountRetirementHost.check(expected.getCause() instanceof IOException, "Wrong write failure");
            }
            AccountRetirementHost.check(h.durable == before, "Failed write replaced durable state");
            AccountRetirementHost.check(h.entries.containsKey(AccountRetirementHost.REMOVED)
                    && h.entries.containsKey(AccountRetirementHost.RETAINED), "Failed write lost an account");
            AccountRetirementHost.check(h.thread.events.isEmpty(), "Failed removal emitted a notification");
            System.out.println("Explicit removal rollback passed: " + AccountRetirementHost.checks + " checks");
        }
    }

    private static String quote(String value) {
        return "\"" + value.replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
    }

    private static void scheduled(String nativeLibrary, String cleaner, String mode) throws Exception {
        LaunchingConnector connector = Bootstrap.virtualMachineManager().defaultConnector();
        Map<String, Connector.Argument> launch = connector.defaultArguments();
        launch.get("main").setValue("AccountExplicitRemovalRaceHost target " + quote(nativeLibrary) + " " + mode);
        launch.get("options").setValue("-XX:-UsePerfData --patch-module " + quote("jdk.unsupported=" + cleaner)
                + " -cp " + quote(System.getProperty("java.class.path")));
        VirtualMachine vm = connector.launch(launch);
        Process child = vm.process();
        Thread out = new Thread(() -> copy(child.getInputStream(), System.out));
        Thread err = new Thread(() -> copy(child.getErrorStream(), System.err));
        out.start(); err.start();
        boolean injected = false;
        try {
            ClassPrepareRequest prepare = vm.eventRequestManager().createClassPrepareRequest();
            prepare.addClassFilter("com.gesher.server.AccountManagerService");
            prepare.enable();
            boolean done = false;
            while (!done) {
                EventSet events;
                try { events = vm.eventQueue().remove(10000); }
                catch (VMDisconnectedException expected) { break; }
                if (events == null) throw new AssertionError("Debugger scheduling boundary timed out");
                for (Event event : events) {
                    if (event instanceof ClassPrepareEvent prepared) {
                        Method method = prepared.referenceType().methodsByName("removeAccountExplicitly").get(0);
                        Location end = method.allLineLocations().stream()
                                .max(Comparator.comparingInt(Location::lineNumber)).orElseThrow();
                        BreakpointRequest point = vm.eventRequestManager().createBreakpointRequest(end);
                        point.setSuspendPolicy(BreakpointRequest.SUSPEND_EVENT_THREAD);
                        point.enable(); prepare.disable();
                    } else if (event instanceof BreakpointEvent boundary) {
                        boundary.request().disable();
                        ClassType fixture = (ClassType)vm.classesByName("AccountExplicitRemovalRaceHost").get(0);
                        fixture.invokeMethod(boundary.thread(), fixture.methodsByName("transition").get(0),
                                List.of(), ObjectReference.INVOKE_SINGLE_THREADED);
                        injected = true;
                    } else if (event instanceof VMDeathEvent || event instanceof VMDisconnectEvent) {
                        done = true;
                    }
                }
                try { events.resume(); } catch (VMDisconnectedException expected) { }
            }
            if (!child.waitFor(10, TimeUnit.SECONDS)) throw new AssertionError("Fixture did not exit");
            out.join(); err.join();
            if (!injected) throw new AssertionError("Scheduling boundary was never reached");
            if (child.exitValue() != 0) throw new AssertionError("Scheduled " + mode + " fixture failed");
        } finally {
            try { vm.dispose(); } catch (VMDisconnectedException expected) { }
            if (child.isAlive()) child.destroyForcibly();
        }
    }

    private static void copy(java.io.InputStream source, java.io.PrintStream destination) {
        try { source.transferTo(destination); }
        catch (IOException failure) { throw new java.io.UncheckedIOException(failure); }
    }

    public static void main(String[] args) throws Exception {
        if (args[0].equals("target")) {
            target(args[1], args[2]);
            return;
        }
        System.load(args[0]);
        android.app.PropertyInvalidatedCache.setTestMode(true);
        failedWrite();
        for (String mode : List.of("unchanged", "update", "reinstall")) scheduled(args[0], args[1], mode);
        System.out.println("Account explicit removal scheduling contracts passed");
    }
}
