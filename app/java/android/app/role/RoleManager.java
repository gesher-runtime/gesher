/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.app.role;

import android.Manifest;
import android.app.compat.CompatChanges;
import android.compat.annotation.ChangeId;
import android.compat.annotation.EnabledSince;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Binder;
import android.os.Process;
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
 * <p>No Android roles are available or held on Gesher. Profile-group active-user
 * operations report their unavailable capability with {@link IllegalStateException}.
 * Other queries return {@code false}, {@code null}, or empty lists after argument
 * and permission checks. Mutations with a result report failure; other void
 * mutations throw {@link UnsupportedOperationException}.
 * {@link #createRequestRoleIntent(String)} builds the documented request-role
 * {@link Intent}. Registered as {@code role} by
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
    private static final String MANAGE_ROLES_FROM_CONTROLLER =
            "com.android.permissioncontroller.permission.MANAGE_ROLES_FROM_CONTROLLER";
    @ChangeId
    @EnabledSince(targetSdkVersion = 35)
    private static final long ROLE_MANAGER_USER_HANDLE_AWARE = 303742236L;

    private final Context mContext;

    RoleManager(Context context) {
        mContext = Objects.requireNonNull(context, "context cannot be null");
    }

    public Intent createRequestRoleIntent(String roleName) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        Intent intent = new Intent(ACTION_REQUEST_ROLE);
        PackageManager pm = mContext.getPackageManager();
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
        enforceUser(getContextUserIdIfAppropriate(), false);
        return false;
    }

    public boolean isRoleHeld(String roleName) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        enforceUser(getContextUserIdIfAppropriate(), false);
        return false;
    }

    /** @hide */
    public void addOnRoleHoldersChangedListenerAsUser(Executor executor,
            OnRoleHoldersChangedListener listener, UserHandle user) {
        Objects.requireNonNull(executor, "executor cannot be null");
        Objects.requireNonNull(listener, "listener cannot be null");
        Objects.requireNonNull(user, "user cannot be null");
        enforceUser(user.getIdentifier(), true);
        enforcePermission(Manifest.permission.OBSERVE_ROLE_HOLDERS);
    }

    /** @hide */
    public void removeOnRoleHoldersChangedListenerAsUser(
            OnRoleHoldersChangedListener listener, UserHandle user) {
        Objects.requireNonNull(listener, "listener cannot be null");
        Objects.requireNonNull(user, "user cannot be null");
        enforceUser(user.getIdentifier(), true);
        enforcePermission(Manifest.permission.OBSERVE_ROLE_HOLDERS);
    }

    /** @hide */
    public void addRoleHolderAsUser(String roleName, String packageName, int flags,
            UserHandle user, Executor executor, Consumer<Boolean> callback) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        Preconditions.checkStringNotEmpty(packageName, "packageName cannot be null or empty");
        Objects.requireNonNull(user, "user cannot be null");
        checkCallback(executor, callback);
        enforceUser(user.getIdentifier(), false);
        enforcePermission(Manifest.permission.MANAGE_ROLE_HOLDERS);
        reportFalse(executor, callback);
    }

    /** @hide */
    public boolean addRoleHolderFromController(String roleName, String packageName) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        Preconditions.checkStringNotEmpty(packageName, "packageName cannot be null or empty");
        enforceController();
        return false;
    }

    /** @hide */
    public void clearRoleHoldersAsUser(String roleName, int flags, UserHandle user,
            Executor executor, Consumer<Boolean> callback) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        Objects.requireNonNull(user, "user cannot be null");
        checkCallback(executor, callback);
        enforceUser(user.getIdentifier(), false);
        enforcePermission(Manifest.permission.MANAGE_ROLE_HOLDERS);
        reportFalse(executor, callback);
    }

    /** @hide */
    public UserHandle getActiveUserForRole(String roleName) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        throw profileGroupUnavailable();
    }

    /** @hide */
    public String getDefaultApplication(String roleName) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        enforceUser(mContext.getUserId(), false);
        enforcePermission(Manifest.permission.MANAGE_DEFAULT_APPLICATIONS);
        checkDefaultRole(roleName);
        return null;
    }

    /** @hide */
    public List<String> getDefaultHoldersForTest(String roleName) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        enforcePermission(Manifest.permission.MANAGE_ROLE_HOLDERS);
        checkTestRole(roleName);
        return List.of();
    }

    /** @hide */
    public List<String> getHeldRolesFromController(String packageName) {
        Preconditions.checkStringNotEmpty(packageName, "packageName cannot be null or empty");
        enforceController();
        return List.of();
    }

    /** @hide */
    public List<String> getRoleHolders(String roleName) {
        return getRoleHoldersAsUser(roleName, UserHandle.of(getContextUserIdIfAppropriate()));
    }

    /** @hide */
    public List<String> getRoleHoldersAsUser(String roleName, UserHandle user) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        Objects.requireNonNull(user, "user cannot be null");
        enforceUser(user.getIdentifier(), false);
        enforceAnyPermission(Manifest.permission.MANAGE_ROLE_HOLDERS,
                Manifest.permission.GET_ROLE_HOLDERS);
        return List.of();
    }

    /** @hide */
    public void isApplicationVisibleForRole(String roleName, String packageName,
            Executor executor, Consumer<Boolean> callback) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        Preconditions.checkStringNotEmpty(packageName, "packageName cannot be null or empty");
        checkCallback(executor, callback);
        enforceUser(getContextUserIdIfAppropriate(), false);
        enforcePermission(Manifest.permission.MANAGE_ROLE_HOLDERS);
        reportFalse(executor, callback);
    }

    /** @hide */
    public boolean isBypassingRoleQualification() {
        enforcePermission(Manifest.permission.MANAGE_ROLE_HOLDERS);
        return false;
    }

    /** @hide */
    public boolean isRoleFallbackEnabled(String roleName) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        enforceUser(mContext.getUserId(), false);
        enforcePermission(Manifest.permission.MANAGE_ROLE_HOLDERS);
        return false;
    }

    /** @hide */
    public void isRoleVisible(String roleName, Executor executor, Consumer<Boolean> callback) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        checkCallback(executor, callback);
        enforceUser(getContextUserIdIfAppropriate(), false);
        enforcePermission(Manifest.permission.MANAGE_ROLE_HOLDERS);
        reportFalse(executor, callback);
    }

    /** @hide */
    public boolean isRoleVisibleForTest(String roleName) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        enforcePermission(Manifest.permission.MANAGE_ROLE_HOLDERS);
        checkTestRole(roleName);
        return false;
    }

    /** @hide */
    public void removeRoleHolderAsUser(String roleName, String packageName, int flags,
            UserHandle user, Executor executor, Consumer<Boolean> callback) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        Preconditions.checkStringNotEmpty(packageName, "packageName cannot be null or empty");
        Objects.requireNonNull(user, "user cannot be null");
        checkCallback(executor, callback);
        enforceUser(user.getIdentifier(), false);
        enforcePermission(Manifest.permission.MANAGE_ROLE_HOLDERS);
        reportFalse(executor, callback);
    }

    /** @hide */
    public boolean removeRoleHolderFromController(String roleName, String packageName) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        Preconditions.checkStringNotEmpty(packageName, "packageName cannot be null or empty");
        enforceController();
        return false;
    }

    /** @hide */
    public void setActiveUserForRole(String roleName, UserHandle user, int flags) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        Objects.requireNonNull(user, "user cannot be null");
        throw profileGroupUnavailable();
    }

    /** @hide */
    public void setBypassingRoleQualification(boolean bypassing) {
        enforcePermission(Manifest.permission.BYPASS_ROLE_QUALIFICATION);
        throw unavailable();
    }

    /** @hide */
    public void setDefaultApplication(String roleName, String packageName, int flags,
            Executor executor, Consumer<Boolean> callback) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        checkCallback(executor, callback);
        enforceUser(mContext.getUserId(), false);
        enforcePermission(Manifest.permission.MANAGE_DEFAULT_APPLICATIONS);
        checkDefaultRole(roleName);
        reportFalse(executor, callback);
    }

    /** @hide */
    public void setDefaultHoldersForTest(String roleName, List<String> packageNames) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        Objects.requireNonNull(packageNames, "packageNames cannot be null");
        enforcePermission(Manifest.permission.MANAGE_ROLE_HOLDERS);
        checkTestRole(roleName);
        throw unavailable();
    }

    /** @hide */
    public void setRoleFallbackEnabled(String roleName, boolean fallbackEnabled) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        enforceUser(mContext.getUserId(), false);
        enforcePermission(Manifest.permission.MANAGE_ROLE_HOLDERS);
        throw unavailable();
    }

    /** @hide */
    public void setRoleNamesFromController(List<String> roleNames) {
        Objects.requireNonNull(roleNames, "roleNames cannot be null");
        enforceController();
        throw unavailable();
    }

    /** @hide */
    public void setRoleVisibleForTest(String roleName, boolean visible) {
        Preconditions.checkStringNotEmpty(roleName, "roleName cannot be null or empty");
        enforcePermission(Manifest.permission.MANAGE_ROLE_HOLDERS);
        checkTestRole(roleName);
        throw unavailable();
    }

    /** @hide */
    public String getBrowserRoleHolder(int userId) {
        enforceUser(userId, false);
        return null;
    }

    /** @hide */
    public String getEmergencyRoleHolder(int userId) {
        enforceUser(userId, false);
        enforcePermission(Manifest.permission.READ_PRIVILEGED_PHONE_STATE);
        return null;
    }

    /** @hide */
    public String getSmsRoleHolder(int userId) {
        enforceUser(userId, false);
        return null;
    }

    /** @hide */
    public boolean setBrowserRoleHolder(String packageName, int userId) {
        enforcePermission(Manifest.permission.SET_PREFERRED_APPLICATIONS);
        enforceUser(userId, false);
        return false;
    }

    private int getContextUserIdIfAppropriate() {
        return CompatChanges.isChangeEnabled(ROLE_MANAGER_USER_HANDLE_AWARE)
                ? mContext.getUserId() : Process.myUserHandle().getIdentifier();
    }

    // This wrapper replaces an outgoing RoleService transaction, whose sender
    // would be this process. An unrelated incoming Binder caller must neither
    // grant this application privileges nor deny its own authorized requests.
    private void enforcePermission(String permission) {
        if (mContext.checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("RoleManager requires " + permission);
        }
    }

    private void enforceAnyPermission(String first, String second) {
        if (mContext.checkSelfPermission(first) != PackageManager.PERMISSION_GRANTED
                && mContext.checkSelfPermission(second)
                        != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("RoleManager requires " + first + " or " + second);
        }
    }

    private void enforceUser(int userId, boolean allowAll) {
        Preconditions.checkArgument(userId >= 0 || (allowAll && userId == UserHandle.USER_ALL),
                "Invalid user " + userId);
        if (userId != Process.myUserHandle().getIdentifier()) {
            enforcePermission(Manifest.permission.INTERACT_ACROSS_USERS_FULL);
        }
    }

    private void enforceController() {
        enforceUser(getContextUserIdIfAppropriate(), false);
        enforcePermission(MANAGE_ROLES_FROM_CONTROLLER);
    }

    private static void checkDefaultRole(String roleName) {
        switch (roleName) {
            case ROLE_ASSISTANT:
            case ROLE_BROWSER:
            case ROLE_CALL_REDIRECTION:
            case ROLE_CALL_SCREENING:
            case ROLE_DIALER:
            case ROLE_HOME:
            case ROLE_SMS:
            case ROLE_WALLET:
                return;
            default:
                throw new IllegalArgumentException("Not a default application role: " + roleName);
        }
    }

    private static void checkTestRole(String roleName) {
        Preconditions.checkArgument(
                ROLE_RESERVED_FOR_TESTING_PROFILE_GROUP_EXCLUSIVITY.equals(roleName),
                "Not a test role: " + roleName);
    }

    private static UnsupportedOperationException unavailable() {
        return new UnsupportedOperationException("Role management is unavailable on this runtime");
    }

    private static IllegalStateException profileGroupUnavailable() {
        // Android 16 RoleService checks profile-group capability before role-domain,
        // user and permission admission. This runtime has no role controller or
        // profile-group role state, so the active-user operation is unavailable.
        return new IllegalStateException("Profile-group roles are unavailable on this runtime");
    }

    private static void checkCallback(Executor executor, Consumer<Boolean> callback) {
        Objects.requireNonNull(executor, "executor cannot be null");
        Objects.requireNonNull(callback, "callback cannot be null");
    }

    private static void reportFalse(Executor executor, Consumer<Boolean> callback) {
        executor.execute(() -> {
            long identity = Binder.clearCallingIdentity();
            try {
                callback.accept(Boolean.FALSE);
            } finally {
                Binder.restoreCallingIdentity(identity);
            }
        });
    }
}
