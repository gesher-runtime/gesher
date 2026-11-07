/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.os.ParcelFileDescriptor;
import java.io.File;
import java.io.IOException;
import java.io.FileDescriptor;

/** Transferable Windows duplex pipe handles, created only in the actual system_server. */
final class InstallPipeTransport {
    private static boolean loaded;
    private InstallPipeTransport() { }

    private static synchronized void load() {
        if (loaded) return;
        String boot = System.getProperty("java.boot.class.path", "");
        for (String entry : boot.split(java.util.regex.Pattern.quote(File.pathSeparator))) {
            if (entry.replace('\\', '/').endsWith("/gesher-app.jar")) {
                System.load(new File(new File(entry).getParentFile(), "libgesher_install_pipe.so").getAbsolutePath());
                loaded = true; return;
            }
        }
        throw new UnsatisfiedLinkError("Cannot locate the Gesher installation pipe library");
    }
    private static native int[] nativeCreatePipe() throws IOException;
    private static native boolean nativeCancelIo(int fd);
    static final InstallFileBridge.Transport TRANSPORT = new InstallFileBridge.Transport() {
        @Override public ParcelFileDescriptor[] create() throws IOException { return InstallPipeTransport.create(); }
        @Override public void cancel(ParcelFileDescriptor server, FileDescriptor target) {
            nativeCancelIo(server.getFd());
            if (target != null && target.valid()) nativeCancelIo(target.getInt$());
        }
    };

    static ParcelFileDescriptor[] create() throws IOException {
        try { load(); }
        catch (LinkageError e) { throw new IOException("Installation pipe transport unavailable", e); }
        int[] fds = nativeCreatePipe();
        if (fds == null || fds.length != 2 || fds[0] < 0 || fds[1] < 0 || fds[0] == fds[1]) {
            throw new IOException("Invalid installation pipe descriptors");
        }
        return new ParcelFileDescriptor[] {ParcelFileDescriptor.adoptFd(fds[0]), ParcelFileDescriptor.adoptFd(fds[1])};
    }
}
