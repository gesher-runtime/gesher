// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

package gesher;

public final class NdkImageReaderTest {
    private static native int nativeRun();
    private static native int nativeRunPrivate();
    private static native int nativeRunDataSpace();

    public static void main(String[] args) {
        System.load(args[0]);
        String mode = args.length > 1 ? args[1] : "";
        int result;
        switch (mode) {
            case "private": result = nativeRunPrivate(); break;
            case "dataspace": result = nativeRunDataSpace(); break;
            default: result = nativeRun();
        }
        if (result != 0) {
            throw new AssertionError("NDK image reader failed");
        }
    }
}
