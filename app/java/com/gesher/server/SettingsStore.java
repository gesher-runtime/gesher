/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;

/** Durable nullable rows. The provider holds its table lock for every access. */
final class SettingsStore {
    private static final String[] TABLES = {"system", "secure", "global"};
    private static final int MAGIC = 0x47535431; // GST1
    private static final int MAX_STRING_LENGTH = 32768;
    private final File mFile;
    private final Map<String, Map<String, String>> mTables;
    private final Map<String, Map<String, Long>> mIds = new LinkedHashMap<>();
    private long mNextId;

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

    static boolean validKey(String name) {
        if (name == null || name.isEmpty()) return false;
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            if (!((c >= 0x20 && c <= 0xd7ff) || (c >= 0xe000 && c <= 0xfffd))) return false;
        }
        return true;
    }

    boolean put(String table, String name, String value, boolean existingOnly) {
        if (!validKey(name)) return false;
        if (name.length() > MAX_STRING_LENGTH || value != null && value.length() > MAX_STRING_LENGTH) {
            throw new IllegalArgumentException("Setting name or value exceeds " + MAX_STRING_LENGTH);
        }
        if ("null".equals(value)) value = null;
        Map<String, String> values = mTables.get(table);
        boolean existed = values.containsKey(name);
        if (existingOnly && !existed) return false;
        String old = values.get(name);
        if (existed && Objects.equals(old, value)) return false;
        Long oldId = mIds.get(table).get(name);
        long nextId = mNextId;
        values.put(name, value);
        // Android SettingsState versions the row on a changed value.
        mIds.get(table).put(name, mNextId++);
        try { persist(); }
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
        try { persist(); }
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

    private void persist() {
        File parent = mFile.getAbsoluteFile().getParentFile();
        File temporary = null;
        try {
            Files.createDirectories(parent.toPath());
            temporary = File.createTempFile("settings-", ".tmp", parent);
            try (FileOutputStream bytes = new FileOutputStream(temporary);
                 DataOutputStream out = new DataOutputStream(bytes)) {
                out.writeInt(MAGIC); out.writeLong(mNextId);
                for (String table : TABLES) {
                    Map<String, String> rows = mTables.get(table);
                    out.writeInt(rows.size());
                    for (Map.Entry<String, String> row : rows.entrySet()) {
                        writeString(out, row.getKey()); out.writeLong(mIds.get(table).get(row.getKey()));
                        out.writeBoolean(row.getValue() != null);
                        if (row.getValue() != null) writeString(out, row.getValue());
                    }
                }
                out.flush(); bytes.getFD().sync();
            }
            try {
                Files.move(temporary.toPath(), mFile.toPath(),
                        StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            } catch (AtomicMoveNotSupportedException unsupported) {
                Files.move(temporary.toPath(), mFile.toPath(), StandardCopyOption.REPLACE_EXISTING);
            }
        } catch (IOException failure) {
            throw new IllegalStateException("Could not persist settings", failure);
        } finally {
            if (temporary != null) temporary.delete();
        }
    }

    private void load() {
        if (!mFile.isFile()) return;
        Map<String, Map<String, String>> tables = new LinkedHashMap<>();
        Map<String, Map<String, Long>> ids = new LinkedHashMap<>();
        try (DataInputStream in = new DataInputStream(new FileInputStream(mFile))) {
            if (in.readInt() != MAGIC) throw new IOException("Unknown settings version");
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
            if (in.read() != -1) throw new IOException("Trailing settings data");
            mTables.clear(); mTables.putAll(tables); mIds.clear(); mIds.putAll(ids); mNextId = nextId;
        } catch (IOException failure) {
            SystemServer.log("settings: durable state unavailable: " + failure);
        }
    }
}
