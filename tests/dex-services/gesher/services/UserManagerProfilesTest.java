/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.Context;
import android.content.ContextWrapper;
import android.content.pm.ApplicationInfo;
import android.content.pm.UserInfo;
import android.os.UserHandle;
import android.os.UserManager;

import com.gesher.server.Services;

import java.util.List;

/** Single-user contracts through the production service and real framework wrappers. */
public final class UserManagerProfilesTest {
    private static int checks;

    private static void check(boolean condition, String reason) {
        if (!condition) throw new AssertionError(reason);
        checks++;
    }

    private static Context context() {
        return new ContextWrapper(null) {
            @Override public int getUserId() { return 0; }
            @Override public Context getApplicationContext() { return this; }
            @Override public ApplicationInfo getApplicationInfo() {
                ApplicationInfo info = new ApplicationInfo();
                info.targetSdkVersion = 35;
                return info;
            }
        };
    }

    private static boolean ownerHandles(List<UserHandle> users) {
        return users != null && users.size() == 1 && users.get(0).getIdentifier() == 0;
    }

    private static boolean owner(UserInfo user) {
        return user != null && user.id == 0 && user.isEnabled() && user.isFull()
                && user.isAdmin() && !user.isProfile() && !user.isManagedProfile()
                && UserManager.USER_TYPE_FULL_SYSTEM.equals(user.userType)
                && (user.flags & UserInfo.FLAG_SYSTEM) != 0;
    }

    private static void profiles() throws Exception {
        Services.UserManager service = new Services.UserManager();
        UserManager framework = new UserManager(context(), service);

        // Regression: Google app called this public framework path, whose array
        // conversion previously crashed on IUserManager.Default's null return.
        check(ownerHandles(framework.getUserProfiles()), "public getUserProfiles includes self");
        check(ownerHandles(framework.getEnabledProfiles()), "enabled profiles include self");
        check(ownerHandles(framework.getAllProfiles()), "all profiles include self");

        for (boolean enabledOnly : new boolean[] {false, true}) {
            int[] ids = service.getProfileIds(0, enabledOnly);
            check(ids != null && ids.length == 1 && ids[0] == 0, "single owner profile IDs");
            List<UserInfo> profiles = service.getProfiles(0, enabledOnly);
            check(profiles != null && profiles.size() == 1 && owner(profiles.get(0)),
                    "profile metadata matches enabled full system owner");
            for (int foreign : new int[] {-1, 1, 10, Integer.MAX_VALUE}) {
                check(service.getProfileIds(foreign, enabledOnly).length == 0,
                        "unknown user has no profile IDs");
                check(service.getProfiles(foreign, enabledOnly).isEmpty(),
                        "unknown user has no profiles");
            }
        }

        check(owner(service.getUserInfo(0)), "user info uses valid explicit system user type");
        check(service.getUserInfo(10) == null, "unknown user has no metadata");
        check(service.isUserRunning(0) && service.isUserUnlocked(0)
                && service.isUserUnlockingOrUnlocked(0), "owner is running and unlocked");
        check(!service.isUserRunning(10) && !service.isUserUnlocked(10)
                && !service.isUserUnlockingOrUnlocked(10), "unknown user is not active");

        int[] changedIds = service.getProfileIds(0, true);
        changedIds[0] = 10;
        check(service.getProfileIds(0, true)[0] == 0, "profile IDs are not shared mutable state");
        UserInfo changedOwner = service.getProfiles(0, true).get(0);
        changedOwner.id = 10;
        changedOwner.flags = UserInfo.FLAG_DISABLED;
        check(owner(service.getProfiles(0, true).get(0)) && owner(service.getUserInfo(0)),
                "profile metadata is not shared mutable state");
        android.os.Bundle restrictions = framework.getApplicationRestrictions("com.gesher.synthetic");
        check(restrictions != null && restrictions.isEmpty(),
                "unmanaged application restrictions are a non-null empty Bundle");
        check(restrictions.getStringArray("whitelisted_google_account_names") == null,
                "absent application account policy stays absent");
        restrictions.putString("synthetic", "changed");
        check(framework.getApplicationRestrictions("com.gesher.synthetic").isEmpty(),
                "application restriction results are not shared mutable state");
        check(service.getApplicationRestrictionsForUser("com.gesher.synthetic", 0).isEmpty()
                && service.getApplicationRestrictionsForUser("com.gesher.synthetic", 10).isEmpty(),
                "no restrictions or metadata are disclosed for unknown users");

        android.os.Bundle userRestrictions = framework.getUserRestrictions(UserHandle.SYSTEM);
        check(userRestrictions != null && userRestrictions.isEmpty(),
                "public user restrictions query returned null instead of an empty Bundle");
        check(!userRestrictions.getBoolean(UserManager.DISALLOW_INSTALL_APPS, false),
                "unmanaged user acquired an installation restriction");
        userRestrictions.putBoolean(UserManager.DISALLOW_INSTALL_APPS, true);
        check(framework.getUserRestrictions(UserHandle.SYSTEM).isEmpty(),
                "user restrictions are shared mutable state");
        check("Owner".equals(framework.getUserName()), "known desktop user has a null/wrong name");
        check(ownerHandles(framework.getUserHandles(false))
                        && ownerHandles(framework.getUserHandles(true)),
                "full user inventory omitted the desktop owner");
        long[] serials = framework.getSerialNumbersOfUsers(false);
        check(serials.length == 1 && serials[0] == 0, "owner serial missing from user inventory");
        check(framework.getSerialNumberForUser(UserHandle.SYSTEM) == 0
                        && UserHandle.SYSTEM.equals(framework.getUserForSerialNumber(0)),
                "owner serial/user mapping is inconsistent");
        for (int unknown : new int[] {-1, 1, 10, Integer.MAX_VALUE}) {
            check(service.getUserSerialNumber(unknown) == -1
                            && service.getUserHandle(unknown) == -1,
                    "unknown user or serial mapped to the owner");
            check(framework.getSerialNumberForUser(UserHandle.of(unknown)) == -1
                            && framework.getUserForSerialNumber(unknown) == null,
                    "public serial query reported an absent user as present");
        }
        UserInfo changedUser = service.getUsers(false).get(0);
        changedUser.id = 10;
        check(owner(service.getUsers(false).get(0)), "user inventory is shared mutable state");
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        profiles();
        System.out.println("UserManagerProfilesTest all passed: " + checks);
    }
}
