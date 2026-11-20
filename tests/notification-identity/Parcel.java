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
        try { while ((bytes.size() & 3) != 0) out.writeByte(0); }
        catch (IOException e) { throw new IllegalStateException(e); }
    }
    public String readString() {
        int length = readInt(); if (length == -1) return null;
        if (length < 0 || length > 1000000) throw new IllegalArgumentException("string size");
        char[] value = new char[length];
        try { for (int i = 0; i < length; ++i) value[i] = in.readChar(); }
        catch (IOException e) { throw new IllegalStateException(e); }
        try { in.skipNBytes((4 - (dataPosition() & 3)) & 3); }
        catch (IOException e) { throw new IllegalStateException(e); }
        return new String(value);
    }
    // The fixture deliberately uses one host encoding for both string methods.
    // Actual framework Intent handles its own fields; this is not Parcel wire proof.
    public void writeString8(String value) { writeString(value); }
    public String readString8() { return readString(); }
    public void writeBundle(Bundle value) {
        if (value != null) throw new UnsupportedOperationException("Host fixture supports null bundles only");
        writeInt(-1);
    }
    public Bundle readBundle() {
        if (readInt() != -1) throw new UnsupportedOperationException("Host fixture supports null bundles only");
        return null;
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
    public int dataSize() { return bytes.size(); }
    public int dataPosition() { return dataSize() - dataAvail(); }
    public int dataAvail() {
        try { return in.available(); } catch (IOException e) { throw new IllegalStateException(e); }
    }
    public void unmarshall(byte[] data, int start, int length) {
        bytes = new ByteArrayOutputStream(); bytes.write(data, start, length);
        // Model native JNI's writeInplace padding explicitly. Production must
        // compare dataSize with the original file length before trusting EOF.
        while ((bytes.size() & 3) != 0) bytes.write(0);
    }
    public void setDataPosition(int position) {
        in = new DataInputStream(new ByteArrayInputStream(bytes.toByteArray()));
        try { in.skipNBytes(position); } catch (IOException e) { throw new IllegalStateException(e); }
    }
}
