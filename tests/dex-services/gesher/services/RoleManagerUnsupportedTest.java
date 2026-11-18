/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.app.role.OnRoleHoldersChangedListener;
import android.app.role.RoleManager;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Process;
import android.os.UserHandle;

import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.List;
import java.util.Set;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Consumer;

/** Optional Role module surface is queryable without an Android role controller. */
public final class RoleManagerUnsupportedTest {
    private static int checks;

    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        ++checks;
    }

    private static void expectInvalidRole(Runnable call, String reason) {
        try {
            call.run();
            throw new AssertionError(reason);
        } catch (IllegalArgumentException | NullPointerException expected) {
            ++checks;
        }
    }

    private static void expect(Class<? extends Throwable> type, Runnable call, String reason) {
        try {
            call.run();
        } catch (Throwable failure) {
            if (!type.isInstance(failure)) throw new AssertionError(reason, failure);
            ++checks;
            return;
        }
        throw new AssertionError(reason);
    }

    private static final String CONTROLLER_PERMISSION =
            "com.android.permissioncontroller.permission.MANAGE_ROLES_FROM_CONTROLLER";
    private static final String TEST_ROLE =
            RoleManager.ROLE_RESERVED_FOR_TESTING_PROFILE_GROUP_EXCLUSIVITY;
    private static final String PACKAGE = "com.example.roleapp";
    private static final String[] ALL_PERMISSIONS = {
            Manifest.permission.MANAGE_ROLE_HOLDERS,
            Manifest.permission.MANAGE_DEFAULT_APPLICATIONS,
            Manifest.permission.OBSERVE_ROLE_HOLDERS,
            Manifest.permission.BYPASS_ROLE_QUALIFICATION,
            Manifest.permission.INTERACT_ACROSS_USERS_FULL,
            Manifest.permission.READ_PRIVILEGED_PHONE_STATE,
            Manifest.permission.SET_PREFERRED_APPLICATIONS,
            CONTROLLER_PERMISSION
    };

    /** Explicit permission boundary double; the service lookup above uses real ContextImpl. */
    private static final class PermissionContext extends ContextWrapper {
        final int userId;
        final Set<String> permissions;
        int permissionChecks;

        PermissionContext(int userId, String... permissions) {
            super(null);
            this.userId = userId;
            this.permissions = Set.of(permissions);
        }

        @Override public int getUserId() { return userId; }
        // A claimed system package name must never substitute for a permission check.
        @Override public String getPackageName() { return "android"; }
        @Override public int checkSelfPermission(String permission) {
            ++permissionChecks;
            return permissions.contains(permission)
                    ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
        }
        @Override public int checkCallingOrSelfPermission(String permission) {
            throw new AssertionError("Local RoleManager used an incoming caller's permissions");
        }
        @Override public void enforceCallingOrSelfPermission(String permission, String message) {
            throw new AssertionError("Local RoleManager used an incoming caller's permissions");
        }
    }

    private static RoleManager manager(PermissionContext context) throws Exception {
        Constructor<RoleManager> constructor = RoleManager.class.getDeclaredConstructor(Context.class);
        constructor.setAccessible(true);
        return constructor.newInstance(context);
    }

    private static final class DeferredExecutor implements Executor {
        final ArrayDeque<Runnable> tasks = new ArrayDeque<>();
        @Override public void execute(Runnable task) { tasks.add(task); }
        void drain() { while (!tasks.isEmpty()) tasks.remove().run(); }
    }

    private interface CallbackOperation {
        void run(RoleManager manager, Executor executor, Consumer<Boolean> callback);
    }

    /** Can also be run on a host JVM with explicitly substituted Binder/Process boundaries. */
    static void contractChecks() throws Exception {
        require(Arrays.binarySearch(com.gesher.server.CompatChanges.disabledFor(34), 303742236L) >= 0,
                "Legacy targets lost RoleManager's process-user compatibility behavior");
        require(Arrays.binarySearch(com.gesher.server.CompatChanges.disabledFor(35), 303742236L) < 0,
                "Modern targets did not enable RoleManager's context-user behavior");
        UserHandle user = Process.myUserHandle();
        int userId = user.getIdentifier();
        int otherUserId = userId + 10;
        UserHandle otherUser = UserHandle.of(otherUserId);
        PermissionContext grantedContext = new PermissionContext(userId, ALL_PERMISSIONS);
        RoleManager roleManager = manager(grantedContext);
        PermissionContext deniedContext = new PermissionContext(userId);
        RoleManager denied = manager(deniedContext);

        // Retain the original absence and failure checks with explicit authorization.
        require(roleManager.getRoleHoldersAsUser(RoleManager.ROLE_SMS, user).isEmpty(),
                "getRoleHoldersAsUser fabricated role holders");
        require(roleManager.getSmsRoleHolder(userId) == null
                        && roleManager.getBrowserRoleHolder(userId) == null
                        && roleManager.getEmergencyRoleHolder(userId) == null,
                "Role holder getters fabricated holders");
        require(!roleManager.setBrowserRoleHolder("com.example.browser", userId),
                "setBrowserRoleHolder reported success");
        require(roleManager.getActiveUserForRole(RoleManager.ROLE_WALLET) == null,
                "getActiveUserForRole fabricated an active user");
        OnRoleHoldersChangedListener listener = (roleName, changedUser) -> {
            throw new AssertionError("Unavailable roles emitted a change");
        };
        roleManager.addOnRoleHoldersChangedListenerAsUser(Runnable::run, listener, user);
        roleManager.removeOnRoleHoldersChangedListenerAsUser(listener, user);
        AtomicBoolean addResult = new AtomicBoolean(true);
        roleManager.addRoleHolderAsUser(RoleManager.ROLE_CALL_SCREENING, "com.nfo.me.android",
                0, user, Runnable::run, addResult::set);
        require(!addResult.get(), "addRoleHolderAsUser reported role grant success");

        require(!denied.isRoleAvailable(RoleManager.ROLE_SMS)
                        && !denied.isRoleHeld(RoleManager.ROLE_SMS),
                "Public role queries require a privileged permission or invent a role");
        require(denied.getBrowserRoleHolder(userId) == null
                        && denied.getSmsRoleHolder(userId) == null,
                "Same-user browser/SMS getters require a privileged permission");

        Runnable[] privilegedCalls = {
                () -> denied.getRoleHolders(RoleManager.ROLE_SMS),
                () -> denied.getRoleHoldersAsUser(RoleManager.ROLE_SMS, user),
                () -> denied.getDefaultApplication(RoleManager.ROLE_SMS),
                () -> denied.getDefaultHoldersForTest(TEST_ROLE),
                () -> denied.getHeldRolesFromController(PACKAGE),
                () -> denied.isBypassingRoleQualification(),
                () -> denied.isRoleFallbackEnabled(RoleManager.ROLE_SMS),
                () -> denied.isRoleVisibleForTest(TEST_ROLE),
                () -> denied.getActiveUserForRole(RoleManager.ROLE_WALLET),
                () -> denied.addRoleHolderFromController(RoleManager.ROLE_SMS, PACKAGE),
                () -> denied.removeRoleHolderFromController(RoleManager.ROLE_SMS, PACKAGE),
                () -> denied.addOnRoleHoldersChangedListenerAsUser(Runnable::run, listener, user),
                () -> denied.removeOnRoleHoldersChangedListenerAsUser(listener, user),
                () -> denied.setActiveUserForRole(RoleManager.ROLE_WALLET, user, 0),
                () -> denied.setBypassingRoleQualification(true),
                () -> denied.setDefaultHoldersForTest(TEST_ROLE, List.of(PACKAGE)),
                () -> denied.setRoleFallbackEnabled(RoleManager.ROLE_SMS, true),
                () -> denied.setRoleNamesFromController(List.of(RoleManager.ROLE_SMS)),
                () -> denied.setRoleVisibleForTest(TEST_ROLE, true),
                () -> denied.getEmergencyRoleHolder(userId),
                () -> denied.setBrowserRoleHolder(PACKAGE, userId)
        };
        for (int i = 0; i < privilegedCalls.length; ++i) {
            expect(SecurityException.class, privilegedCalls[i], "Unprivileged call admitted: " + i);
        }
        require(deniedContext.permissionChecks > 0 && grantedContext.permissionChecks > 0,
                "Role APIs did not consult the supplied Context permission boundary");

        CallbackOperation[] operations = {
                (r, e, c) -> r.addRoleHolderAsUser(RoleManager.ROLE_SMS, PACKAGE, 0, user, e, c),
                (r, e, c) -> r.removeRoleHolderAsUser(RoleManager.ROLE_SMS, PACKAGE, 0, user, e, c),
                (r, e, c) -> r.clearRoleHoldersAsUser(RoleManager.ROLE_SMS, 0, user, e, c),
                (r, e, c) -> r.setDefaultApplication(RoleManager.ROLE_SMS, null, 0, e, c),
                (r, e, c) -> r.isRoleVisible(RoleManager.ROLE_SMS, e, c),
                (r, e, c) -> r.isApplicationVisibleForRole(RoleManager.ROLE_SMS, PACKAGE, e, c)
        };
        for (CallbackOperation operation : operations) {
            DeferredExecutor executor = new DeferredExecutor();
            AtomicInteger calls = new AtomicInteger();
            Consumer<Boolean> callback = success -> {
                if (!Boolean.FALSE.equals(success)) throw new AssertionError("Fabricated success");
                calls.incrementAndGet();
            };
            expect(NullPointerException.class, () -> operation.run(denied, executor, null),
                    "Null callback was accepted or permission was checked before required arguments");
            expect(NullPointerException.class, () -> operation.run(denied, null, callback),
                    "Null executor was accepted or permission was checked before required arguments");
            expect(SecurityException.class, () -> operation.run(denied, executor, callback),
                    "Unprivileged asynchronous call did not throw");
            require(calls.get() == 0 && executor.tasks.isEmpty(),
                    "Rejected asynchronous call dispatched a result");
            operation.run(roleManager, executor, callback);
            require(calls.get() == 0 && executor.tasks.size() == 1,
                    "Result bypassed the supplied deferred executor or was not dispatched");
            executor.drain();
            require(calls.get() == 1 && executor.tasks.isEmpty(),
                    "Authorized unsupported call must report false exactly once");
        }

        Runnable[] unavailableMutations = {
                () -> roleManager.setActiveUserForRole(RoleManager.ROLE_WALLET, user, 0),
                () -> roleManager.setBypassingRoleQualification(true),
                () -> roleManager.setBypassingRoleQualification(false),
                () -> roleManager.setDefaultHoldersForTest(TEST_ROLE, List.of(PACKAGE)),
                () -> roleManager.setRoleFallbackEnabled(RoleManager.ROLE_SMS, true),
                () -> roleManager.setRoleNamesFromController(List.of(RoleManager.ROLE_SMS)),
                () -> roleManager.setRoleNamesFromController(List.of()),
                () -> roleManager.setRoleVisibleForTest(TEST_ROLE, true)
        };
        for (int i = 0; i < unavailableMutations.length; ++i) {
            expect(UnsupportedOperationException.class, unavailableMutations[i],
                    "Void mutation falsely completed: " + i);
        }
        require(!roleManager.isBypassingRoleQualification()
                        && !roleManager.isRoleFallbackEnabled(RoleManager.ROLE_SMS)
                        && !roleManager.isRoleVisibleForTest(TEST_ROLE)
                        && roleManager.getDefaultHoldersForTest(TEST_ROLE).isEmpty()
                        && roleManager.getHeldRolesFromController(PACKAGE).isEmpty(),
                "Rejected mutation created role state");
        require(!roleManager.addRoleHolderFromController(RoleManager.ROLE_SMS, PACKAGE)
                        && !roleManager.removeRoleHolderFromController(RoleManager.ROLE_SMS, PACKAGE),
                "Controller mutation fabricated success");

        RoleManager readOnly = manager(new PermissionContext(userId, Manifest.permission.GET_ROLE_HOLDERS));
        require(readOnly.getRoleHoldersAsUser(RoleManager.ROLE_SMS, user).isEmpty()
                        && readOnly.getRoleHolders(RoleManager.ROLE_SMS).isEmpty(),
                "GET_ROLE_HOLDERS cannot perform its read-only queries");
        expect(SecurityException.class,
                () -> readOnly.addRoleHolderAsUser(RoleManager.ROLE_SMS, PACKAGE, 0, user,
                        Runnable::run, result -> { }), "GET_ROLE_HOLDERS admitted a mutation");
        expect(SecurityException.class, () -> readOnly.getDefaultApplication(RoleManager.ROLE_SMS),
                "Role holder read permission admitted default-app management");

        RoleManager manageOnly = manager(new PermissionContext(userId, Manifest.permission.MANAGE_ROLE_HOLDERS));
        expect(SecurityException.class, () -> manageOnly.getActiveUserForRole(RoleManager.ROLE_WALLET),
                "Active-user query omitted the full cross-user permission");
        expect(SecurityException.class, () -> manageOnly.setBypassingRoleQualification(true),
                "Holder management permission admitted qualification bypass");
        expect(SecurityException.class, () -> manageOnly.setRoleNamesFromController(List.of()),
                "Holder management permission admitted controller-only mutation");
        expect(SecurityException.class, () -> manageOnly.getDefaultApplication(RoleManager.ROLE_SMS),
                "Holder management permission admitted default-app API");
        expect(SecurityException.class, () -> manageOnly.getEmergencyRoleHolder(userId),
                "Holder management permission admitted privileged phone-state query");
        expect(SecurityException.class, () -> manageOnly.setBrowserRoleHolder(PACKAGE, userId),
                "Holder management permission admitted preferred-app setter");
        expect(SecurityException.class,
                () -> manageOnly.addOnRoleHoldersChangedListenerAsUser(Runnable::run, listener, user),
                "Holder management permission admitted observation");

        RoleManager defaults = manager(new PermissionContext(userId,
                Manifest.permission.MANAGE_DEFAULT_APPLICATIONS));
        for (String role : new String[] {RoleManager.ROLE_ASSISTANT, RoleManager.ROLE_BROWSER,
                RoleManager.ROLE_CALL_REDIRECTION, RoleManager.ROLE_CALL_SCREENING,
                RoleManager.ROLE_DIALER, RoleManager.ROLE_HOME, RoleManager.ROLE_SMS,
                RoleManager.ROLE_WALLET}) {
            require(defaults.getDefaultApplication(role) == null, "Default role domain rejected " + role);
        }
        for (String role : new String[] {RoleManager.ROLE_SYSTEM_ACTIVITY_RECOGNIZER,
                RoleManager.ROLE_NOTES, "example.UNKNOWN_ROLE"}) {
            expect(IllegalArgumentException.class, () -> defaults.getDefaultApplication(role),
                    "Non-default role accepted by getDefaultApplication");
            expect(IllegalArgumentException.class,
                    () -> defaults.setDefaultApplication(role, PACKAGE, 0, Runnable::run,
                            result -> { throw new AssertionError("Invalid role dispatched callback"); }),
                    "Non-default role accepted by setDefaultApplication");
        }
        RoleManager defaultsAndCrossUser = manager(new PermissionContext(userId,
                Manifest.permission.MANAGE_DEFAULT_APPLICATIONS,
                Manifest.permission.INTERACT_ACROSS_USERS_FULL));
        require(defaultsAndCrossUser.getActiveUserForRole(RoleManager.ROLE_WALLET) == null,
                "Active-user query rejected the default-app permission alternative");
        expect(UnsupportedOperationException.class,
                () -> defaultsAndCrossUser.setActiveUserForRole(RoleManager.ROLE_WALLET, user, 0),
                "Active-user setter rejected authorized default-app management");

        RoleManager observer = manager(new PermissionContext(userId,
                Manifest.permission.OBSERVE_ROLE_HOLDERS));
        observer.addOnRoleHoldersChangedListenerAsUser(Runnable::run, listener, user);
        observer.removeOnRoleHoldersChangedListenerAsUser(listener, user);
        expect(SecurityException.class,
                () -> observer.addOnRoleHoldersChangedListenerAsUser(Runnable::run, listener, UserHandle.ALL),
                "Observing all users omitted cross-user authorization");
        roleManager.addOnRoleHoldersChangedListenerAsUser(Runnable::run, listener, UserHandle.ALL);
        roleManager.removeOnRoleHoldersChangedListenerAsUser(listener, UserHandle.ALL);

        expect(SecurityException.class, () -> denied.getBrowserRoleHolder(otherUserId),
                "Cross-user browser query admitted without permission");
        expect(SecurityException.class, () -> denied.getSmsRoleHolder(otherUserId),
                "Cross-user SMS query admitted without permission");
        expect(SecurityException.class, () -> manageOnly.getRoleHoldersAsUser(RoleManager.ROLE_SMS, otherUser),
                "Role management permission admitted cross-user read");
        RoleManager weakCrossUser = manager(new PermissionContext(userId,
                Manifest.permission.INTERACT_ACROSS_USERS, Manifest.permission.MANAGE_ROLE_HOLDERS));
        expect(SecurityException.class,
                () -> weakCrossUser.getRoleHoldersAsUser(RoleManager.ROLE_SMS, otherUser),
                "Weaker cross-user permission substituted for INTERACT_ACROSS_USERS_FULL");
        require(roleManager.getBrowserRoleHolder(otherUserId) == null
                        && roleManager.getSmsRoleHolder(otherUserId) == null
                        && roleManager.getRoleHoldersAsUser(RoleManager.ROLE_SMS, otherUser).isEmpty(),
                "Authorized absent cross-user roles invented state");
        RoleManager otherContext = manager(new PermissionContext(otherUserId,
                Manifest.permission.MANAGE_DEFAULT_APPLICATIONS));
        expect(SecurityException.class, () -> otherContext.getDefaultApplication(RoleManager.ROLE_SMS),
                "Context user was ignored by default-app read");
        expect(SecurityException.class,
                () -> otherContext.setDefaultApplication(RoleManager.ROLE_SMS, null, 0,
                        Runnable::run, result -> { throw new AssertionError("Cross-user callback"); }),
                "Context user was ignored by default-app mutation");

        expect(IllegalArgumentException.class,
                () -> roleManager.getRoleHoldersAsUser(RoleManager.ROLE_SMS, UserHandle.ALL),
                "Non-listener query accepted USER_ALL");
        expect(IllegalArgumentException.class,
                () -> roleManager.addOnRoleHoldersChangedListenerAsUser(Runnable::run, listener,
                        UserHandle.CURRENT), "Listener accepted unsupported special user");
        expect(IllegalArgumentException.class, () -> roleManager.getDefaultHoldersForTest(RoleManager.ROLE_SMS),
                "Test holder query accepted a production role");
        expect(IllegalArgumentException.class, () -> roleManager.setRoleVisibleForTest(RoleManager.ROLE_SMS, true),
                "Test visibility setter accepted a production role");

        expect(RejectedExecutionException.class,
                () -> roleManager.clearRoleHoldersAsUser(RoleManager.ROLE_SMS, 0, user,
                        task -> { throw new RejectedExecutionException("test executor"); },
                        result -> { throw new AssertionError("Rejected executor ran callback"); }),
                "Executor rejection was swallowed");
        expect(IllegalStateException.class,
                () -> roleManager.clearRoleHoldersAsUser(RoleManager.ROLE_SMS, 0, user, Runnable::run,
                        result -> { throw new IllegalStateException("test callback"); }),
                "Callback failure was swallowed");
    }

    public static void main(String[] args) throws Exception {
        String root = System.getenv("ANDROID_ROOT");
        com.gesher.app.Main.bootstrap("role-manager-test", root + "/build.prop",
                root + "/framework/framework-res.apk");
        com.gesher.server.Services.installInProcess(com.gesher.server.Services.create(List.of()));

        Class<?> threadType = Class.forName("android.app.ActivityThread");
        Constructor<?> constructor = threadType.getDeclaredConstructor();
        constructor.setAccessible(true);
        Object thread = constructor.newInstance();
        Method create = Class.forName("android.app.ContextImpl")
                .getDeclaredMethod("createSystemContext", threadType);
        create.setAccessible(true);
        Context context = (Context) create.invoke(null, thread);

        RoleManager roleManager = context.getSystemService(RoleManager.class);
        require(roleManager != null, "Typed Context lookup did not create RoleManager");
        require(roleManager == context.getSystemService(Context.ROLE_SERVICE),
                "String and typed Context.ROLE_SERVICE lookups differ");
        require(RoleManager.class.getClassLoader() == Object.class.getClassLoader(),
                "RoleManager is not on the boot class path");
        require(RoleManager.class.getConstructors().length == 0,
                "RoleManager should not expose a public constructor");

        require("android.app.role.CALL_SCREENING".equals(RoleManager.ROLE_CALL_SCREENING),
                "ROLE_CALL_SCREENING constant differs");
        require("android.app.role.DIALER".equals(RoleManager.ROLE_DIALER),
                "ROLE_DIALER constant differs");
        require("android.app.role.BROWSER".equals(RoleManager.ROLE_BROWSER),
                "ROLE_BROWSER constant differs");
        require("android.app.role.SMS".equals(RoleManager.ROLE_SMS),
                "ROLE_SMS constant differs");
        require("android.app.role.HOME".equals(RoleManager.ROLE_HOME),
                "ROLE_HOME constant differs");

        require(!roleManager.isRoleAvailable(RoleManager.ROLE_CALL_SCREENING),
                "Unavailable runtime reported ROLE_CALL_SCREENING available");
        require(!roleManager.isRoleHeld(RoleManager.ROLE_CALL_SCREENING),
                "Unavailable runtime reported ROLE_CALL_SCREENING held");
        require(!roleManager.isRoleAvailable(RoleManager.ROLE_DIALER)
                        && !roleManager.isRoleHeld(RoleManager.ROLE_DIALER),
                "Unavailable runtime reported ROLE_DIALER available or held");

        Intent intent = roleManager.createRequestRoleIntent(RoleManager.ROLE_CALL_SCREENING);
        require(intent != null && "android.app.role.action.REQUEST_ROLE".equals(intent.getAction()),
                "createRequestRoleIntent action differs");
        require(RoleManager.ROLE_CALL_SCREENING.equals(intent.getStringExtra(Intent.EXTRA_ROLE_NAME)),
                "createRequestRoleIntent did not set Intent.EXTRA_ROLE_NAME");

        expectInvalidRole(() -> roleManager.isRoleAvailable(null), "isRoleAvailable accepted null");
        expectInvalidRole(() -> roleManager.isRoleHeld(""), "isRoleHeld accepted empty role");
        expectInvalidRole(() -> roleManager.createRequestRoleIntent(""),
                "createRequestRoleIntent accepted empty role");

        contractChecks();

        System.out.println("RoleManagerUnsupportedTest all passed: " + checks + " synthetic checks");
    }
}
