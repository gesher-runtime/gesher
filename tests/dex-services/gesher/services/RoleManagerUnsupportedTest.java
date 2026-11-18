/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.role.OnRoleHoldersChangedListener;
import android.app.role.RoleManager;
import android.content.Context;
import android.content.Intent;
import android.os.Process;
import android.os.UserHandle;

import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

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

        UserHandle user = Process.myUserHandle();
        require(roleManager.getRoleHoldersAsUser(RoleManager.ROLE_SMS, user).isEmpty(),
                "getRoleHoldersAsUser fabricated role holders");
        require(roleManager.getSmsRoleHolder(0) == null
                        && roleManager.getBrowserRoleHolder(0) == null
                        && roleManager.getEmergencyRoleHolder(0) == null,
                "Role holder getters fabricated holders");
        require(!roleManager.setBrowserRoleHolder("com.example.browser", 0),
                "setBrowserRoleHolder reported success");
        require(roleManager.getActiveUserForRole(RoleManager.ROLE_WALLET) == null,
                "getActiveUserForRole fabricated an active user");

        OnRoleHoldersChangedListener listener = (roleName, changedUser) -> { };
        roleManager.addOnRoleHoldersChangedListenerAsUser(Runnable::run, listener, user);
        roleManager.removeOnRoleHoldersChangedListenerAsUser(listener, user);
        AtomicBoolean addResult = new AtomicBoolean(true);
        roleManager.addRoleHolderAsUser(RoleManager.ROLE_CALL_SCREENING, "com.nfo.me.android",
                0, user, Runnable::run, addResult::set);
        require(!addResult.get(), "addRoleHolderAsUser reported role grant success");

        System.out.println("RoleManagerUnsupportedTest all passed: " + checks + " synthetic checks");
    }
}
