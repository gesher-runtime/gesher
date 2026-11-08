/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package android.app.sdksandbox;

/**
 * Capability query for the optional SDK sandbox module, absent from this runtime.
 * No sandbox service or SDK execution API is supplied. A real module earlier on
 * the boot class path takes precedence over this unavailable-capability type.
 */
public final class SdkSandboxManager {
    public static final String SDK_SANDBOX_SERVICE = "sdk_sandbox";
    public static final int SDK_SANDBOX_STATE_DISABLED = 0;
    public static final int SDK_SANDBOX_STATE_ENABLED_PROCESS_ISOLATION = 2;

    private SdkSandboxManager() {}

    /** Gesher does not provide isolated SDK processes. */
    public static int getSdkSandboxState() {
        return SDK_SANDBOX_STATE_DISABLED;
    }
}
