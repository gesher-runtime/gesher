/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.UUID;

/** One durable, immutable enabled-state snapshot for the single system-server writer. */
final class PackageEnabledStore {
    // The values are Android's COMPONENT_ENABLED_STATE_* constants; this storage layer
    // has no framework/native dependencies so crash and I/O behavior is host-testable.
    record State(int application, Map<String, Integer> components) {
        static final State DEFAULT = new State(0, Map.of());
        State {
            if (application < 0 || application > 4) throw new IllegalArgumentException("Invalid application state");
            Map<String, Integer> copy = new LinkedHashMap<>();
            for (var entry : components.entrySet()) {
                if (entry.getKey() == null || entry.getKey().isEmpty()
                        || entry.getValue() == null || entry.getValue() < 1 || entry.getValue() > 2) {
                    throw new IllegalArgumentException("Invalid component state");
                }
                copy.put(entry.getKey(), entry.getValue());
            }
            components = Collections.unmodifiableMap(copy);
        }
        int component(String name) { return components.getOrDefault(name, 0); }
        State application(int value) {
            return application == value ? this : new State(value, components);
        }
        State component(String name, int value) {
            if (value < 0 || value > 2) throw new IllegalArgumentException("Invalid component state");
            if (component(name) == value) return this;
            Map<String, Integer> next = new LinkedHashMap<>(components);
            if (value == 0) next.remove(name); else next.put(name, value);
            return new State(application, next);
        }
    }
    record Entry(String owner, State state) { }

    private static final int MAGIC = 0x47455331; // GES1
    private static final int MAX_PACKAGES = 20000;
    private static final int MAX_COMPONENTS = 100000;
    private static final long MAX_BYTES = 32L * 1024 * 1024;
    private final File file;
    private final Path owners;
    private final Map<String, String> transientOwners;
    private volatile Map<String, Entry> entries;

    PackageEnabledStore(File file, Path owners) {
        this.file = file;
        this.owners = owners;
        transientOwners = null;
        entries = read();
    }

    /** Standalone app services must not write the running device's package policy. */
    PackageEnabledStore() {
        file = null; owners = null;
        transientOwners = new LinkedHashMap<>();
        entries = Map.of();
    }

    private static void packageName(String name) {
        if (name == null || !name.matches("[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)*")) {
            throw new IllegalArgumentException("Invalid enabled-state package name");
        }
    }
    private static String readOwner(Path marker) throws IOException {
        if (Files.isSymbolicLink(marker) || !Files.isRegularFile(marker, LinkOption.NOFOLLOW_LINKS)
                || Files.size(marker) != 36) throw new IOException("Invalid enabled-state owner marker");
        String value = Files.readString(marker, StandardCharsets.US_ASCII);
        if (!UUID.fromString(value).toString().equals(value)) throw new IOException("Invalid enabled-state owner");
        return value;
    }

    /** The marker survives updates and is moved by the installer's removal quarantine. */
    synchronized String owner(String name) {
        packageName(name);
        if (transientOwners != null) {
            return transientOwners.computeIfAbsent(name, ignored -> UUID.randomUUID().toString());
        }
        Path marker = owners.resolve(name);
        Path temporary = null;
        try {
            if (Files.exists(marker, LinkOption.NOFOLLOW_LINKS)) return readOwner(marker);
            Files.createDirectories(owners);
            temporary = Files.createTempFile(owners, ".owner-", ".tmp");
            String value = UUID.randomUUID().toString();
            try (FileOutputStream out = new FileOutputStream(temporary.toFile())) {
                out.write(value.getBytes(StandardCharsets.US_ASCII)); out.getFD().sync();
            }
            // A same-filesystem rename publishes a complete marker; never replace an
            // existing installation's marker if a second constructor got there first.
            try { Files.move(temporary, marker); }
            catch (java.nio.file.FileAlreadyExistsException raced) { return readOwner(marker); }
            return value;
        } catch (IOException | IllegalArgumentException failure) {
            throw new IllegalStateException("Cannot establish enabled-state owner for " + name, failure);
        } finally {
            if (temporary != null) try { Files.deleteIfExists(temporary); } catch (IOException ignored) { }
        }
    }

    State state(String name, String owner) {
        Entry entry = entries.get(name);
        return entry != null && entry.owner().equals(owner) ? entry.state() : State.DEFAULT;
    }

    /** Persist all changes before the single volatile publication point. */
    synchronized void commit(Map<String, Entry> changes) {
        if (changes.isEmpty()) return;
        Map<String, Entry> next = new LinkedHashMap<>(entries);
        next.putAll(changes);
        if (file != null) persist(next);
        entries = Collections.unmodifiableMap(next);
    }

    /** No stale disk row can authorize a later install once its marker is retired. */
    synchronized void retire(String name, String owner) {
        packageName(name);
        if (transientOwners != null) {
            transientOwners.remove(name, owner);
        } else {
            Path marker = owners.resolve(name);
            try {
                if (Files.exists(marker, LinkOption.NOFOLLOW_LINKS) && readOwner(marker).equals(owner)) {
                    Files.delete(marker);
                }
            } catch (IOException failure) {
                throw new IllegalStateException("Cannot retire enabled-state owner for " + name, failure);
            }
        }
        Map<String, Entry> next = new LinkedHashMap<>(entries);
        Entry previous = next.get(name);
        if (previous != null && previous.owner().equals(owner)) next.remove(name);
        entries = Collections.unmodifiableMap(next);
        // The marker is the durable retirement point. Obsolete rows are reclaimed by
        // the next successful snapshot commit; uninstall need not rewrite this file.
    }

    private void persist(Map<String, Entry> snapshot) {
        Path temporary = null;
        try {
            if (snapshot.size() > MAX_PACKAGES) throw new IOException("Too many enabled-state packages");
            for (var item : snapshot.entrySet()) {
                packageName(item.getKey());
                Entry entry = item.getValue();
                if (entry == null || entry.owner() == null || entry.state() == null
                        || !UUID.fromString(entry.owner()).toString().equals(entry.owner())) {
                    throw new IOException("Invalid enabled-state entry");
                }
                if (entry.state().components().size() > MAX_COMPONENTS) {
                    throw new IOException("Too many enabled-state components");
                }
            }
            Path parent = file.getAbsoluteFile().getParentFile().toPath();
            Files.createDirectories(parent);
            temporary = Files.createTempFile(parent, "enabled-state-", ".tmp");
            try (FileOutputStream bytes = new FileOutputStream(temporary.toFile());
                    DataOutputStream out = new DataOutputStream(new BufferedOutputStream(bytes))) {
                out.writeInt(MAGIC); out.writeInt(snapshot.size());
                for (var item : snapshot.entrySet()) {
                    out.writeUTF(item.getKey()); out.writeUTF(item.getValue().owner());
                    State state = item.getValue().state();
                    out.writeInt(state.application()); out.writeInt(state.components().size());
                    for (var component : state.components().entrySet()) {
                        out.writeUTF(component.getKey()); out.writeInt(component.getValue());
                    }
                }
                out.flush(); bytes.getFD().sync();
            }
            if (Files.size(temporary) > MAX_BYTES) throw new IOException("Enabled-state snapshot is too large");
            // Do not fall back to delete-then-rename: an unavailable atomic replacement
            // is an explicit failure, with both the old snapshot and memory untouched.
            Files.move(temporary, file.toPath(), StandardCopyOption.ATOMIC_MOVE,
                    StandardCopyOption.REPLACE_EXISTING);
        } catch (IOException | IllegalArgumentException failure) {
            throw new IllegalStateException("Cannot persist package enabled state", failure);
        } finally {
            if (temporary != null) try { Files.deleteIfExists(temporary); } catch (IOException ignored) { }
        }
    }

    private Map<String, Entry> read() {
        if (!Files.exists(file.toPath(), LinkOption.NOFOLLOW_LINKS)) return Map.of();
        try {
            if (Files.isSymbolicLink(file.toPath()) || !file.isFile() || file.length() > MAX_BYTES) {
                throw new IOException("Invalid enabled-state file");
            }
            try (DataInputStream in = new DataInputStream(new BufferedInputStream(new FileInputStream(file)))) {
                if (in.readInt() != MAGIC) throw new IOException("Unknown enabled-state version");
                int count = in.readInt();
                if (count < 0 || count > MAX_PACKAGES) throw new IOException("Invalid package count");
                Map<String, Entry> result = new LinkedHashMap<>();
                for (int i = 0; i < count; i++) {
                    String name = in.readUTF(), owner = in.readUTF(); packageName(name);
                    if (!UUID.fromString(owner).toString().equals(owner)) throw new IOException("Invalid owner");
                    int application = in.readInt(), size = in.readInt();
                    if (size < 0 || size > MAX_COMPONENTS) throw new IOException("Invalid component count");
                    Map<String, Integer> components = new LinkedHashMap<>();
                    for (int j = 0; j < size; j++) {
                        if (components.put(in.readUTF(), in.readInt()) != null) throw new IOException("Duplicate component");
                    }
                    if (result.put(name, new Entry(owner, new State(application, components))) != null) {
                        throw new IOException("Duplicate package");
                    }
                }
                if (in.read() != -1) throw new IOException("Trailing enabled-state data");
                // A committed uninstall can retire an owner before PMS cleans the
                // aggregate snapshot. Ignore those rows, and reclaim them on the next
                // commit, so restart/uninstall churn cannot retain or revive them.
                var retained = new LinkedHashMap<String, Entry>();
                for (var item : result.entrySet()) {
                    Path marker = owners.resolve(item.getKey());
                    if (Files.exists(marker, LinkOption.NOFOLLOW_LINKS)
                            && readOwner(marker).equals(item.getValue().owner())) {
                        retained.put(item.getKey(), item.getValue());
                    }
                }
                return Collections.unmodifiableMap(retained);
            }
        } catch (IOException | IllegalArgumentException failure) {
            // Silently dropping a corrupt store would re-enable intentionally disabled apps.
            throw new IllegalStateException("Cannot read package enabled state", failure);
        }
    }
}
