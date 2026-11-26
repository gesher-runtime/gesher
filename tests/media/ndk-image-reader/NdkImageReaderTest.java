// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

package gesher;

public final class NdkImageReaderTest {
    private static native int nativeRun();

    public static void main(String[] args) {
        System.load(args[0]);
        if (nativeRun() != 0) {
            throw new AssertionError("NDK image reader failed");
        }
    }
}
