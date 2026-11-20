/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.app.role;

import android.os.UserHandle;

/** Listener for changes to the holders of any role for a user. */
public interface OnRoleHoldersChangedListener {
    void onRoleHoldersChanged(String roleName, UserHandle user);
}
