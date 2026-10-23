/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.app.sdksandbox.sandboxactivity;

import android.content.Context;
import android.content.Intent;

/**
 * Placeholder for the SDK sandbox module's class, which framework.jar is compiled against
 * but does not contain (app/README.md, "Classes framework.jar does not have"):
 * ActivityThread.performLaunchActivity asks it whether an intent starts an SDK sandbox
 * activity. There is no SDK sandbox: no intent does. When the module's class is on the
 * boot class path ahead of gesher-app.jar, it wins.
 */
public abstract class SdkSandboxActivityAuthority {
    private static final SdkSandboxActivityAuthority INSTANCE =
            new SdkSandboxActivityAuthority() {};

    protected SdkSandboxActivityAuthority() {
    }

    public static SdkSandboxActivityAuthority getInstance() {
        return INSTANCE;
    }

    public static boolean isSdkSandboxActivityIntent(Context context, Intent intent) {
        return false;
    }

    public ActivityContextInfo getActivityContextInfo(Intent intent) {
        throw new IllegalArgumentException("there is no SDK sandbox: " + intent);
    }
}
