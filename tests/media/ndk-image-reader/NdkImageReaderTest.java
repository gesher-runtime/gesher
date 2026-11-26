// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

package gesher;

public final class NdkImageReaderTest {
    private static native int nativeRun();
    private static native int nativeRunPrivate();

    public static void main(String[] args) {
        System.load(args[0]);
        int result = args.length > 1 && args[1].equals("private") ? nativeRunPrivate() : nativeRun();
        if (result != 0) {
            throw new AssertionError("NDK image reader failed");
        }
    }
}
