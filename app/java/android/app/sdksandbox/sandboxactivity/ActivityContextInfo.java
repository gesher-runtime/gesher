/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.app.sdksandbox.sandboxactivity;

import android.content.pm.ApplicationInfo;

/** Placeholder for the SDK sandbox module's interface (see SdkSandboxActivityAuthority). */
public interface ActivityContextInfo {
    default int getContextFlags() {
        return 0;
    }

    ApplicationInfo getSdkApplicationInfo();
}
