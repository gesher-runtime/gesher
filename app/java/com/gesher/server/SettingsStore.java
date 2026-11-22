/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.EOFException;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;
import java.util.zip.CRC32;
import java.util.zip.CheckedInputStream;
import java.util.zip.CheckedOutputStream;

/**
 * Durable nullable rows. The provider holds its table lock for every access.
 *
 * <p>GST2 stores a checksummed snapshot followed by checksummed, length-delimited row
 * mutations. Every mutation is synced before it succeeds; no timer defers durability.
 * Replacing the whole file checkpoints the journal, so a crash cannot pair a newer
 * snapshot with an older journal. GST1 snapshots are migrated on the first change.
 */
final class SettingsStore {
    private static final String[] TABLES = {"system", "secure", "global"};
    private static final int MAGIC = 0x47535431; // GST1
    private static final int JOURNAL_MAGIC = 0x47535432; // GST2
    private static final int MAX_STRING_LENGTH = 32768;
    private static final int MAX_RECORD_LENGTH = 4 * MAX_STRING_LENGTH + 32;
    private static final int MAX_JOURNAL_RECORDS = 128;
    private static final int MAX_JOURNAL_BYTES = 1024 * 1024;
    private final File mFile;
    private final Map<String, Map<String, String>> mTables;
    private final Map<String, Map<String, Long>> mIds = new LinkedHashMap<>();
    private long mNextId;
    private boolean mCheckpointNeeded = true;
    private boolean mTornTail;
    private long mValidLength;
    private int mJournalRecords;
    private int mJournalBytes;
    private IOException mWriteFailure;

    SettingsStore(File file, Map<String, Map<String, String>> defaults) {
        mFile = file;
        mTables = defaults;
        for (String table : TABLES) {
            Map<String, Long> ids = new LinkedHashMap<>();
            for (String name : defaults.get(table).keySet()) ids.put(name, mNextId++);
            mIds.put(table, ids);
        }
        load();
    }

    long id(String table, String name) { return mIds.get(table).get(name); }

    void addDefault(String table, String name, String value) {
        Map<String, String> values = mTables.get(table);
        if (values.containsKey(name)) return;
        if (mNextId == Long.MAX_VALUE) throw new IllegalStateException("Settings generations exhausted");
        values.put(name, value);
        mIds.get(table).put(name, mNextId++);
        // A later row-only append would otherwise omit newly introduced defaults.
        mCheckpointNeeded = true;
    }

    static boolean validKey(String name) {
        if (name == null || name.isEmpty()) return false;
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            if (!((c >= 0x20 && c <= 0xd7ff) || (c >= 0xe000 && c <= 0xfffd))) return false;
        }
        return true;
    }

    boolean put(String table, String name, String value, boolean existingOnly) {
        return put(table, name, value, existingOnly, false);
    }

    boolean put(String table, String name, String value, boolean existingOnly, boolean newGeneration) {
        if (!validKey(name)) return false;
        if (name.length() > MAX_STRING_LENGTH || value != null && value.length() > MAX_STRING_LENGTH) {
            throw new IllegalArgumentException("Setting name or value exceeds " + MAX_STRING_LENGTH);
        }
        if ("null".equals(value)) value = null;
        Map<String, String> values = mTables.get(table);
        boolean existed = values.containsKey(name);
        if (existingOnly && !existed) return false;
        String old = values.get(name);
        if (existed && Objects.equals(old, value) && !newGeneration) return false;
        if (mNextId == Long.MAX_VALUE) throw new IllegalStateException("Settings generations exhausted");
        Long oldId = mIds.get(table).get(name);
        long nextId = mNextId;
        values.put(name, value);
        // Android SettingsState versions the row on a changed value. A reused
        // checker subtype ID also needs a new row generation for the new provider.
        mIds.get(table).put(name, mNextId++);
        try { persist(table, name, value, false); }
        catch (RuntimeException failure) {
            if (existed) { values.put(name, old); mIds.get(table).put(name, oldId); }
            else { values.remove(name); mIds.get(table).remove(name); }
            mNextId = nextId;
            throw failure;
        }
        return true;
    }

    boolean remove(String table, String name) {
        if (!validKey(name)) return false;
        Map<String, String> values = mTables.get(table);
        if (!values.containsKey(name)) return false;
        String old = values.remove(name);
        Long oldId = mIds.get(table).remove(name);
        try { persist(table, name, null, true); }
        catch (RuntimeException failure) {
            values.put(name, old); mIds.get(table).put(name, oldId); throw failure;
        }
        return true;
    }

    // UTF-16 units preserve binary values and unpaired surrogates as SettingsState does.
    private static void writeString(DataOutputStream out, String value) throws IOException {
        out.writeInt(value.length());
        for (int i = 0; i < value.length(); i++) out.writeChar(value.charAt(i));
    }
    private static String readString(DataInputStream in) throws IOException {
        int length = in.readInt();
        if (length < 0 || length > MAX_STRING_LENGTH) throw new IOException("Invalid setting length");
        char[] value = new char[length];
        for (int i = 0; i < length; i++) value[i] = in.readChar();
        return new String(value);
    }

    private void persist(String table, String name, String value, boolean remove) {
        if (mWriteFailure != null) {
            throw new IllegalStateException("Settings storage requires recovery", mWriteFailure);
        }
        try {
            byte[] record = record(table, name, value, remove);
            if (mCheckpointNeeded || mJournalRecords >= MAX_JOURNAL_RECORDS
                    || mJournalBytes + record.length > MAX_JOURNAL_BYTES) {
                checkpoint();
            } else {
                append(record);
            }
        } catch (IOException failure) {
            throw new IllegalStateException("Could not persist settings", failure);
        }
    }

    /** At most one changed row is buffered, independently of the size of the store. */
    private byte[] record(String table, String name, String value, boolean remove) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        DataOutputStream out = new DataOutputStream(bytes);
        out.writeLong(mNextId);
        int index = java.util.Arrays.asList(TABLES).indexOf(table);
        out.writeByte(index);
        out.writeByte(remove ? 2 : value == null ? 0 : 1);
        writeString(out, name);
        if (!remove && value != null) writeString(out, value);
        out.flush();
        byte[] payload = bytes.toByteArray();
        CRC32 checksum = new CRC32();
        checksum.update(payload);
        bytes.reset();
        out.writeInt(payload.length);
        // Validate framing before treating EOF as an incomplete payload. Without
        // this redundant header, a corrupted length could hide a committed row
        // as a supposedly torn append and the next write would truncate it.
        out.writeInt(~payload.length);
        out.write(payload);
        out.writeInt((int) checksum.getValue());
        out.flush();
        return bytes.toByteArray();
    }

    /**
     * A failed append is truncated and synced before the caller rolls its tables back.
     * If rollback itself fails, refuse every further write: appending after uncertain
     * bytes could acknowledge changes that replay cannot reach.
     */
    private void append(byte[] record) throws IOException {
        RandomAccessFile bytes = new RandomAccessFile(mFile, "rw");
        boolean committed = false;
        try {
            if (mTornTail) {
                bytes.setLength(mValidLength);
                bytes.getFD().sync();
                mTornTail = false;
            } else if (bytes.length() != mValidLength) {
                throw new IOException("Settings file changed outside its owner");
            }
            bytes.seek(mValidLength);
            try {
                bytes.write(record);
                bytes.getFD().sync();
                committed = true;
            } catch (IOException failure) {
                try {
                    bytes.setLength(mValidLength);
                    bytes.getFD().sync();
                } catch (IOException rollback) {
                    failure.addSuppressed(rollback);
                    mWriteFailure = failure;
                }
                throw failure;
            }
            mValidLength += record.length;
            mJournalBytes += record.length;
            mJournalRecords++;
        } finally {
            try { bytes.close(); }
            catch (IOException close) {
                // The durability boundary is sync, not closing an already synced FD.
                // Reporting failure after commit would roll back only the in-memory row.
                if (!committed) throw close;
                SystemServer.log("settings: closing committed journal failed: " + close);
            }
        }
    }

    private void checkpoint() throws IOException {
        File parent = mFile.getAbsoluteFile().getParentFile();
        File temporary = null;
        try {
            Files.createDirectories(parent.toPath());
            temporary = File.createTempFile("settings-", ".tmp", parent);
            CRC32 checksum = new CRC32();
            try (FileOutputStream bytes = new FileOutputStream(temporary);
                 DataOutputStream out = new DataOutputStream(new CheckedOutputStream(
                         new BufferedOutputStream(bytes), checksum))) {
                out.writeInt(JOURNAL_MAGIC); out.writeLong(mNextId);
                for (String table : TABLES) {
                    Map<String, String> rows = mTables.get(table);
                    out.writeInt(rows.size());
                    for (Map.Entry<String, String> row : rows.entrySet()) {
                        writeString(out, row.getKey()); out.writeLong(mIds.get(table).get(row.getKey()));
                        out.writeBoolean(row.getValue() != null);
                        if (row.getValue() != null) writeString(out, row.getValue());
                    }
                }
                out.writeInt((int) checksum.getValue());
                out.flush(); bytes.getFD().sync();
            }
            try {
                Files.move(temporary.toPath(), mFile.toPath(),
                        StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            } catch (AtomicMoveNotSupportedException unsupported) {
                Files.move(temporary.toPath(), mFile.toPath(), StandardCopyOption.REPLACE_EXISTING);
            }
            mValidLength = mFile.length();
            mJournalRecords = mJournalBytes = 0;
            mCheckpointNeeded = mTornTail = false;
        } finally {
            if (temporary != null) temporary.delete();
        }
    }

    private void load() {
        if (!mFile.isFile()) return;
        Map<String, Map<String, String>> tables = new LinkedHashMap<>();
        Map<String, Map<String, Long>> ids = new LinkedHashMap<>();
        boolean journal = false;
        try (InputStream bytes = new BufferedInputStream(new FileInputStream(mFile))) {
            CountedInput counted = new CountedInput(bytes);
            CRC32 checksum = new CRC32();
            DataInputStream in = new DataInputStream(new CheckedInputStream(counted, checksum));
            int magic = in.readInt();
            journal = magic == JOURNAL_MAGIC;
            if (!journal && magic != MAGIC) throw new IOException("Unknown settings version");
            long nextId = in.readLong();
            if (nextId < 0) throw new IOException("Invalid setting generation");
            for (String table : TABLES) {
                Map<String, String> values = new LinkedHashMap<>();
                Map<String, Long> rowIds = new LinkedHashMap<>();
                int count = in.readInt();
                if (count < 0 || count > 1000000) throw new IOException("Invalid row count");
                for (int i = 0; i < count; i++) {
                    String name = readString(in); long id = in.readLong();
                    if (!validKey(name) || rowIds.containsKey(name) || id < 0 || id >= nextId) {
                        throw new IOException("Invalid settings row");
                    }
                    rowIds.put(name, id); values.put(name, in.readBoolean() ? readString(in) : null);
                }
                tables.put(table, values); ids.put(table, rowIds);
            }
            if (journal) {
                int expected = (int) checksum.getValue();
                if (in.readInt() != expected) throw new IOException("Invalid settings snapshot checksum");
                mValidLength = counted.count;
                // Only the checkpoint is covered by the first checksum. Each suffix
                // record carries its own checksum, and replay publishes complete rows.
                nextId = replay(new DataInputStream(counted), counted, tables, ids, nextId);
                mCheckpointNeeded = false;
            } else if (in.read() != -1) {
                throw new IOException("Trailing settings data");
            }
            mTables.clear(); mTables.putAll(tables); mIds.clear(); mIds.putAll(ids); mNextId = nextId;
        } catch (IOException failure) {
            // An incomplete final append is handled by replay. A corrupt complete
            // journal/checkpoint must never silently turn acknowledged rows into defaults.
            if (journal) throw new IllegalStateException("Corrupt settings journal", failure);
            SystemServer.log("settings: durable state unavailable: " + failure);
        }
    }

    private long replay(DataInputStream in, CountedInput counted,
            Map<String, Map<String, String>> tables, Map<String, Map<String, Long>> ids,
            long nextId) throws IOException {
        while (true) {
            int first = in.read();
            if (first < 0) return nextId;
            byte[] payload;
            int expected;
            try {
                int length = (first << 24) | (in.readUnsignedByte() << 16)
                        | (in.readUnsignedByte() << 8) | in.readUnsignedByte();
                int complement = in.readInt();
                if (complement != ~length || length < 16 || length > MAX_RECORD_LENGTH) {
                    throw new IOException("Invalid settings record length");
                }
                payload = new byte[length];
                in.readFully(payload);
                expected = in.readInt();
            } catch (EOFException torn) {
                mTornTail = true;
                return nextId;
            }
            CRC32 checksum = new CRC32();
            checksum.update(payload);
            if ((int) checksum.getValue() != expected) throw new IOException("Invalid settings record checksum");
            DataInputStream row = new DataInputStream(new ByteArrayInputStream(payload));
            long followingId = row.readLong();
            int tableIndex = row.readUnsignedByte();
            int operation = row.readUnsignedByte();
            if (tableIndex >= TABLES.length || operation > 2) throw new IOException("Invalid settings operation");
            String name = readString(row);
            String value = operation == 1 ? readString(row) : null;
            if (!validKey(name) || row.read() != -1) throw new IOException("Invalid settings mutation");
            Map<String, String> values = tables.get(TABLES[tableIndex]);
            Map<String, Long> rowIds = ids.get(TABLES[tableIndex]);
            if (operation == 2) {
                if (followingId != nextId || !values.containsKey(name)) throw new IOException("Invalid settings removal");
                values.remove(name); rowIds.remove(name);
            } else {
                if (nextId == Long.MAX_VALUE || followingId != nextId + 1) {
                    throw new IOException("Invalid settings generation");
                }
                values.put(name, value); rowIds.put(name, nextId);
            }
            nextId = followingId;
            mValidLength = counted.count;
            mJournalBytes += payload.length + 12;
            mJournalRecords++;
            if (mJournalRecords > MAX_JOURNAL_RECORDS || mJournalBytes > MAX_JOURNAL_BYTES) {
                throw new IOException("Settings journal exceeds checkpoint bounds");
            }
        }
    }

    /** Counts decoded bytes, not buffered read-ahead, to locate the last complete record. */
    private static final class CountedInput extends InputStream {
        final InputStream input;
        long count;
        CountedInput(InputStream input) { this.input = input; }
        @Override public int read() throws IOException {
            int result = input.read();
            if (result >= 0) count++;
            return result;
        }
        @Override public int read(byte[] buffer, int offset, int length) throws IOException {
            int result = input.read(buffer, offset, length);
            if (result > 0) count += result;
            return result;
        }
    }
}
