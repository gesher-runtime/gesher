// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Test-only SharedMemory JNI surface backed by actual Linux fd/mmap operations.
package android.os;
import bitmaphost.Host;
import java.io.FileDescriptor;
import java.nio.ByteBuffer;
public final class SharedMemory implements AutoCloseable {
    private final FileDescriptor descriptor;
    public static boolean failClose;
    private SharedMemory(FileDescriptor fd) { descriptor = fd; }
    public static SharedMemory create(String name, int size) { return new SharedMemory(Host.createFd(size)); }
    public FileDescriptor getFileDescriptor() { return descriptor; }
    public ByteBuffer mapReadWrite() { return Host.mapFd(descriptor, true); }
    public ByteBuffer mapReadOnly() { return Host.mapFd(descriptor, false); }
    public static void unmap(ByteBuffer buffer) { Host.unmap(buffer); }
    public void writeToParcel(Parcel parcel, int flags) { parcel.writeFileDescriptor(descriptor); }
    public void close() {
        Host.closeFd(descriptor);
        if (failClose) { failClose = false; throw new RuntimeException("close fault"); }
    }
    public static final Parcelable.Creator<SharedMemory> CREATOR = parcel -> new SharedMemory(parcel.readRawFileDescriptor());
}
