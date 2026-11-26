// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
package gesher;

public final class NdkCodecTest {
    private static native int run();

    public static void main(String[] args) {
        System.load(args[0]);
        if (run() != 0) throw new AssertionError("NDK AVC round trip failed");
    }
}
