/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package android.os;
import java.io.*;

/** Explicit host serialization seam. This is NOT Android's Parcel wire format.
 * Production NotificationStore controls all fields, files, identity checks and migration. */
public final class Parcel {
    private ByteArrayOutputStream bytes = new ByteArrayOutputStream();
    private DataOutputStream out = new DataOutputStream(bytes);
    private DataInputStream in;
    public static Parcel obtain() { return new Parcel(); }
    public void recycle() { }
    public void writeInt(int n) { try { out.writeInt(n); } catch (IOException e) { throw new IllegalStateException(e); } }
    public int readInt() { try { return in.readInt(); } catch (IOException e) { throw new IllegalStateException(e); } }
    public void writeString(String value) {
        writeInt(value == null ? -1 : value.length());
        if (value != null) for (int i = 0; i < value.length(); ++i) {
            try { out.writeChar(value.charAt(i)); } catch (IOException e) { throw new IllegalStateException(e); }
        }
    }
    public String readString() {
        int length = readInt(); if (length == -1) return null;
        if (length < 0 || length > 1000000) throw new IllegalArgumentException("string size");
        char[] value = new char[length];
        try { for (int i = 0; i < length; ++i) value[i] = in.readChar(); }
        catch (IOException e) { throw new IllegalStateException(e); }
        return new String(value);
    }
    public void writeStringArray(String[] value) {
        writeInt(value == null ? -1 : value.length);
        if (value != null) for (String item : value) writeString(item);
    }
    public String[] createStringArray() {
        int count = readInt(); if (count == -1) return null;
        String[] result = new String[count];
        for (int i = 0; i < count; ++i) result[i] = readString();
        return result;
    }
    public byte[] marshall() { return bytes.toByteArray(); }
    public void unmarshall(byte[] data, int start, int length) {
        bytes = new ByteArrayOutputStream(); bytes.write(data, start, length);
    }
    public void setDataPosition(int position) {
        in = new DataInputStream(new ByteArrayInputStream(bytes.toByteArray()));
        try { in.skipNBytes(position); } catch (IOException e) { throw new IllegalStateException(e); }
    }
}
