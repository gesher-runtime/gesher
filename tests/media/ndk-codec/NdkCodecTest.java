// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
package gesher;

public final class NdkCodecTest {
    private static native int run();
    private static native int runAsync();

    public static void main(String[] args) {
        System.load(args[0]);
        boolean async = args.length > 1 && args[1].equals("async");
        if ((async ? runAsync() : run()) != 0) throw new AssertionError("NDK AVC round trip failed");
    }
}
