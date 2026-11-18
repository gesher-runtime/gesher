/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.app.role;

import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.UserHandle;
import com.android.internal.util.Preconditions;

import java.util.List;
import java.util.Objects;
import java.util.concurrent.Executor;
import java.util.function.Consumer;

/**
 * RoleManager for a runtime without an Android role controller service.
 * framework.jar compiles against the Permission/Role module's API stubs and does not
 * contain it; apps that query {@code Context.ROLE_SERVICE} or link against
 * {@code RoleManager} (e.g. caller-ID / dialer / browser role checks) failed with
 * {@link NoClassDefFoundError}.
 *
 * <p>No Android roles are available or held on Gesher. Queries return {@code false},
 * {@code null}, or empty lists, and {@link #createRequestRoleIntent(String)} builds
 * the documented request-role {@link Intent}. Registered as {@code role} by
 * {@link RoleFrameworkInitializer}.
 */
public final class RoleManager {
    public static final String ROLE_ASSISTANT = "android.app.role.ASSISTANT";
    public static final String ROLE_BROWSER = "android.app.role.BROWSER";
    public static final String ROLE_CALL_REDIRECTION = "android.app.role.CALL_REDIRECTION";
    public static final String ROLE_CALL_SCREENING = "android.app.role.CALL_SCREENING";
    public static final String ROLE_CONTENT_RESTRICTION = "android.app.role.CONTENT_RESTRICTION";
    public static final String ROLE_DIALER = "android.app.role.DIALER";
    public static final String ROLE_EMERGENCY = "android.app.role.EMERGENCY";
    public static final String ROLE_HOME = "android.app.role.HOME";
    public static final String ROLE_NOTES = "android.app.role.NOTES";
    public static final String ROLE_SMS = "android.app.role.SMS";
    public static final String ROLE_WALLET = "android.app.role.WALLET";

    public static final int MANAGE_HOLDERS_FLAG_DONT_KILL_APP = 1;

    public static final String ROLE_DEVICE_POLICY_MANAGEMENT =
            "android.app.role.DEVICE_POLICY_MANAGEMENT";
    public static final String ROLE_FINANCED_DEVICE_KIOSK =
            "android.app.role.FINANCED_DEVICE_KIOSK";
    public static final String ROLE_RESERVED_FOR_TESTING_PROFILE_GROUP_EXCLUSIVITY =
            "android.app.role.RESERVED_FOR_TESTING_PROFILE_GROUP_EXCLUSIVITY";
    public static final String ROLE_SUPERVISION = "android.app.role.SUPERVISION";
    public static final String ROLE_SYSTEM_ACTIVITY_RECOGNIZER =
            "android.app.role.SYSTEM_ACTIVITY_RECOGNIZER";
    public static final String ROLE_SYSTEM_CALL_STREAMING =
            "android.app.role.SYSTEM_CALL_STREAMING";
    public static final String ROLE_SYSTEM_DEPENDENCY_INSTALLER =
            "android.app.role.SYSTEM_DEPENDENCY_INSTALLER";
    public static final String ROLE_SYSTEM_FINANCED_DEVICE_CONTROLLER =
            "android.app.role.SYSTEM_FINANCED_DEVICE_CONTROLLER";
    public static final String ROLE_SYSTEM_SUPERVISION =
            "android.app.role.SYSTEM_SUPERVISION";
    public static final String ROLE_SYSTEM_WELLBEING = "android.app.role.SYSTEM_WELLBEING";

    private static final String ACTION_REQUEST_ROLE = "android.app.role.action.REQUEST_ROLE";

    private final Context mContext;

    RoleManager(Context context) {
        mContext = context;
    }

    public Intent createRequestRoleIntent(String roleName) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        Intent intent = new Intent(ACTION_REQUEST_ROLE);
        PackageManager pm = mContext != null ? mContext.getPackageManager() : null;
        if (pm != null) {
            String permissionController = pm.getPermissionControllerPackageName();
            if (permissionController != null) {
                intent.setPackage(permissionController);
            }
        }
        intent.putExtra(Intent.EXTRA_ROLE_NAME, roleName);
        return intent;
    }

    public boolean isRoleAvailable(String roleName) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        return false;
    }

    public boolean isRoleHeld(String roleName) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        return false;
    }

    /** @hide */
    public void addOnRoleHoldersChangedListenerAsUser(Executor executor,
            OnRoleHoldersChangedListener listener, UserHandle user) {
        Objects.requireNonNull(executor, "executor cannot be null");
        Objects.requireNonNull(listener, "listener cannot be null");
        Objects.requireNonNull(user, "user cannot be null");
    }

    /** @hide */
    public void removeOnRoleHoldersChangedListenerAsUser(
            OnRoleHoldersChangedListener listener, UserHandle user) {
        Objects.requireNonNull(listener, "listener cannot be null");
        Objects.requireNonNull(user, "user cannot be null");
    }

    /** @hide */
    public void addRoleHolderAsUser(String roleName, String packageName, int flags,
            UserHandle user, Executor executor, Consumer<Boolean> callback) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        Preconditions.checkStringNotEmpty(packageName, "packageName cannot be null or empty");
        Objects.requireNonNull(user, "user cannot be null");
        reportFalse(executor, callback);
    }

    /** @hide */
    public boolean addRoleHolderFromController(String roleName, String packageName) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        Preconditions.checkStringNotEmpty(packageName, "packageName cannot be null or empty");
        return false;
    }

    /** @hide */
    public void clearRoleHoldersAsUser(String roleName, int flags, UserHandle user,
            Executor executor, Consumer<Boolean> callback) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        Objects.requireNonNull(user, "user cannot be null");
        reportFalse(executor, callback);
    }

    /** @hide */
    public UserHandle getActiveUserForRole(String roleName) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        return null;
    }

    /** @hide */
    public String getDefaultApplication(String roleName) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        return null;
    }

    /** @hide */
    public List<String> getDefaultHoldersForTest(String roleName) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        return List.of();
    }

    /** @hide */
    public List<String> getHeldRolesFromController(String packageName) {
        Preconditions.checkStringNotEmpty(packageName, "packageName cannot be null or empty");
        return List.of();
    }

    /** @hide */
    public List<String> getRoleHolders(String roleName) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        return List.of();
    }

    /** @hide */
    public List<String> getRoleHoldersAsUser(String roleName, UserHandle user) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        Objects.requireNonNull(user, "user cannot be null");
        return List.of();
    }

    /** @hide */
    public void isApplicationVisibleForRole(String roleName, String packageName,
            Executor executor, Consumer<Boolean> callback) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        Preconditions.checkStringNotEmpty(packageName, "packageName cannot be null or empty");
        Objects.requireNonNull(callback, "callback cannot be null");
        reportFalse(executor, callback);
    }

    /** @hide */
    public boolean isBypassingRoleQualification() {
        return false;
    }

    /** @hide */
    public boolean isRoleFallbackEnabled(String roleName) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        return false;
    }

    /** @hide */
    public void isRoleVisible(String roleName, Executor executor, Consumer<Boolean> callback) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        Objects.requireNonNull(callback, "callback cannot be null");
        reportFalse(executor, callback);
    }

    /** @hide */
    public boolean isRoleVisibleForTest(String roleName) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        return false;
    }

    /** @hide */
    public void removeRoleHolderAsUser(String roleName, String packageName, int flags,
            UserHandle user, Executor executor, Consumer<Boolean> callback) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        Preconditions.checkStringNotEmpty(packageName, "packageName cannot be null or empty");
        Objects.requireNonNull(user, "user cannot be null");
        reportFalse(executor, callback);
    }

    /** @hide */
    public boolean removeRoleHolderFromController(String roleName, String packageName) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        Preconditions.checkStringNotEmpty(packageName, "packageName cannot be null or empty");
        return false;
    }

    /** @hide */
    public void setActiveUserForRole(String roleName, UserHandle user, int flags) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        Objects.requireNonNull(user, "user cannot be null");
    }

    /** @hide */
    public void setBypassingRoleQualification(boolean bypassing) {
    }

    /** @hide */
    public void setDefaultApplication(String roleName, String packageName, int flags,
            Executor executor, Consumer<Boolean> callback) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        reportFalse(executor, callback);
    }

    /** @hide */
    public void setDefaultHoldersForTest(String roleName, List<String> packageNames) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        Objects.requireNonNull(packageNames, "packageNames cannot be null");
    }

    /** @hide */
    public void setRoleFallbackEnabled(String roleName, boolean fallbackEnabled) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
    }

    /** @hide */
    public void setRoleNamesFromController(List<String> roleNames) {
        Objects.requireNonNull(roleNames, "roleNames cannot be null");
    }

    /** @hide */
    public void setRoleVisibleForTest(String roleName, boolean visible) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
    }

    /** @hide */
    public String getBrowserRoleHolder(int userId) {
        return null;
    }

    /** @hide */
    public String getEmergencyRoleHolder(int userId) {
        return null;
    }

    /** @hide */
    public String getSmsRoleHolder(int userId) {
        return null;
    }

    /** @hide */
    public boolean setBrowserRoleHolder(String packageName, int userId) {
        return false;
    }

    private static void reportFalse(Executor executor, Consumer<Boolean> callback) {
        Objects.requireNonNull(executor, "executor cannot be null");
        if (callback != null) {
            executor.execute(() -> callback.accept(Boolean.FALSE));
        }
    }
}
