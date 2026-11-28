/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher;

public final class VulkanCoreTest {
    private static native int run(boolean coreOnly);

    public static void main(String[] args) {
        if (args.length != 1) throw new IllegalArgumentException("Pass the native fixture path");
        System.load(args[0]);
        int status = run(true);
        if (status != 0) throw new AssertionError("Vulkan core contract failed: " + status);
    }
}
