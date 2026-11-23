/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;
import java.io.IOException;

/** Windows-owned recursive directory stream; all tokens have explicit close ownership. */
final class DexoptProfileNotifications {
    private DexoptProfileNotifications() { }
    static native long open(String path) throws IOException;
    static native String[] next(long watch) throws IOException;
    static native void wake(long watch);
    static native void close(long watch);
}
