/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.admin.DevicePolicyManager;
import android.app.admin.IDevicePolicyManager;
import android.app.admin.StartInstallingUpdateCallback;
import android.content.ComponentName;
import android.os.ParcelFileDescriptor;
import android.os.RemoteCallback;
import android.os.UserHandle;

/** No Android device/profile owner; Windows disk encryption is not Android FBE. */
class UnmanagedDevicePolicyService extends IDevicePolicyManager.Default {
    private static SecurityException denied() {
        return new SecurityException("This Windows environment has no Android device or profile "
                + "owner and cannot authorize device administration");
    }

    @Override public boolean isAdminActive(ComponentName admin, int userId) { return false; }
    // Android documents null, rather than an empty list, when no administrators exist.
    @Override public java.util.List<ComponentName> getActiveAdmins(int userId) { return null; }
    @Override public boolean hasGrantedPolicy(ComponentName admin, int policy, int userId) {
        return false;
    }
    @Override public boolean hasDeviceOwner() { return false; }
    @Override public boolean isDeviceManaged() { return false; }
    @Override public boolean isOrganizationOwnedDeviceWithManagedProfile() { return false; }
    @Override public ComponentName getDeviceOwnerComponent(boolean callingUserOnly) { return null; }
    @Override public ComponentName getDeviceOwnerComponentOnUser(int userId) { return null; }
    @Override public String getDeviceOwnerName() { return null; }
    @Override public int getDeviceOwnerUserId() { return UserHandle.USER_NULL; }
    @Override public ComponentName getProfileOwnerAsUser(int userId) { return null; }
    @Override public ComponentName getProfileOwnerOrDeviceOwnerSupervisionComponent(UserHandle user) {
        return null;
    }
    @Override public String getProfileOwnerName(int userId) { return null; }
    @Override public int getUserProvisioningState(int userId) {
        return DevicePolicyManager.STATE_USER_UNMANAGED;
    }
    @Override public boolean isProvisioningAllowed(String action, String packageName) {
        return false;
    }
    @Override public int getStorageEncryptionStatus(String caller, int userId) {
        return DevicePolicyManager.ENCRYPTION_STATUS_UNSUPPORTED;
    }
    @Override public boolean getStorageEncryption(ComponentName admin, int userId) { return false; }

    @Override public void setActiveAdmin(ComponentName admin, boolean refreshing, int userId,
            String caller) { throw denied(); }
    @Override public void removeActiveAdmin(ComponentName admin, int userId) { throw denied(); }
    @Override public void forceRemoveActiveAdmin(ComponentName admin, int userId) { throw denied(); }
    @Override public boolean setDeviceOwner(ComponentName admin, int userId, boolean onlySetup) {
        throw denied();
    }
    @Override public void clearDeviceOwner(String packageName) { throw denied(); }
    @Override public boolean setProfileOwner(ComponentName admin, int userId) { throw denied(); }
    @Override public void clearProfileOwner(ComponentName admin) { throw denied(); }
    @Override public void lockNow(int flags, String caller, boolean parent) { throw denied(); }
    @Override public void wipeDataWithReason(String caller, int flags, String reason,
            boolean parent, boolean factoryReset) { throw denied(); }
    @Override public boolean resetPassword(String password, int flags) { throw denied(); }
    @Override public int setStorageEncryption(ComponentName admin, boolean encrypt) { throw denied(); }
    @Override public void setPermissionPolicy(ComponentName admin, String caller, int policy) {
        throw denied();
    }
    @Override public int getPermissionPolicy(ComponentName admin) { return DevicePolicyManager.PERMISSION_POLICY_PROMPT; }
    @Override public int getPermissionGrantState(ComponentName admin, String caller, String pkg,
            String permission) { return DevicePolicyManager.PERMISSION_GRANT_STATE_DEFAULT; }
    @Override public void setPermissionGrantState(ComponentName admin, String caller, String pkg,
            String permission, int state, RemoteCallback result) {
        // The framework waits on this callback for 20 seconds. A denied caller must
        // receive the exception before it starts waiting, not a Default no-op.
        throw denied();
    }
    @Override public void getRemoveWarning(ComponentName admin, RemoteCallback result, int userId) {
        throw denied();
    }
    @Override public void installUpdateFromFile(ComponentName admin, String caller,
            ParcelFileDescriptor file, StartInstallingUpdateCallback callback) { throw denied(); }
    @Override public void setAuditLogEventsCallback(String caller,
            android.app.admin.IAuditLogEventsCallback callback) { throw denied(); }
}
