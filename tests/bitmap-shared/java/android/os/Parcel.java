// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Test-only Java transport; every FD slot and read owns a real POSIX duplicate.
package android.os;
import bitmaphost.Host;
import java.io.FileDescriptor;
import java.util.ArrayList;
public final class Parcel implements AutoCloseable {
    public final ArrayList<Object> values = new ArrayList<>();
    public int position, failAt = -1, calls;
    private boolean allowFds = true;
    public boolean failFdAfterDup;
    public boolean pushAllowFds(boolean value) { boolean old = allowFds; allowFds = value; return old; }
    public void restoreAllowFds(boolean value) { allowFds = value; }
    private void step() { if (calls++ == failAt) throw new RuntimeException("parcel fault"); }
    public void writeInt(int value) { step(); values.add(value); }
    public void writeLong(long value) { step(); values.add(value); }
    public void writeByteArray(byte[] value) { step(); values.add(value.clone()); }
    public int readInt() { step(); return (Integer) values.get(position++); }
    public long readLong() { step(); return (Long) values.get(position++); }
    public byte[] createByteArray() { step(); return ((byte[]) values.get(position++)).clone(); }
    public void writeFileDescriptor(FileDescriptor value) {
        step();
        if (!allowFds) throw new IllegalStateException("fds forbidden");
        values.add(Host.dupFd(value));
        if (failFdAfterDup) throw new RuntimeException("fd slot fault");
    }
    public FileDescriptor readRawFileDescriptor() { step(); return Host.dupFd((FileDescriptor) values.get(position++)); }
    public void rewind() { position = 0; calls = 0; failAt = -1; }
    public boolean hasFileDescriptors() { for (Object item : values) if (item instanceof FileDescriptor) return true; return false; }
    public void close() {
        for (Object item : values) if (item instanceof FileDescriptor) Host.closeFd((FileDescriptor) item);
        values.clear();
    }
}
