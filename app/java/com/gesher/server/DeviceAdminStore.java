/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.io.*;
import java.nio.file.*;
import java.util.*;

/** Durable grants bound to the installer's per-installation owner, not a package name. */
final class DeviceAdminStore {
    record Grant(String owner, int policies) { }
    private final Path file;
    private Map<String, Grant> grants;
    DeviceAdminStore(Path file) { this.file = file; grants = read(); }
    synchronized Grant get(String admin) { return grants.get(admin); }
    synchronized Map<String, Grant> snapshot() { return Map.copyOf(grants); }
    synchronized void put(String admin, Grant grant) {
        Map<String, Grant> next = new TreeMap<>(grants);
        if (grant == null) next.remove(admin); else next.put(admin, grant);
        if (file != null) {
            Path temporary = file.resolveSibling(file.getFileName() + ".tmp");
            try {
                Files.createDirectories(file.getParent());
                try (FileOutputStream stream = new FileOutputStream(temporary.toFile());
                        DataOutputStream out = new DataOutputStream(stream)) {
                    out.writeInt(0x47444131); out.writeInt(next.size());
                    for (var item : next.entrySet()) {
                        out.writeUTF(item.getKey()); out.writeUTF(item.getValue().owner());
                        out.writeInt(item.getValue().policies());
                    }
                    out.flush(); stream.getFD().sync();
                }
                Files.move(temporary, file, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            } catch (IOException failure) {
                throw new IllegalStateException("Device administrator grant could not be saved", failure);
            } finally {
                try { Files.deleteIfExists(temporary); } catch (IOException ignored) { }
            }
        }
        grants = next;
    }
    private Map<String, Grant> read() {
        if (file == null || !Files.exists(file)) return Map.of();
        try {
            if (Files.isSymbolicLink(file) || Files.size(file) > 4 * 1024 * 1024) return Map.of();
            try (DataInputStream in = new DataInputStream(Files.newInputStream(file))) {
                if (in.readInt() != 0x47444131) return Map.of();
                int count = in.readInt();
                if (count < 0 || count > 10000) return Map.of();
                Map<String, Grant> result = new TreeMap<>();
                for (int i = 0; i < count; i++) {
                    String admin = in.readUTF(), owner = in.readUTF(); int policies = in.readInt();
                    if (admin.length() > 1024 || !UUID.fromString(owner).toString().equals(owner)
                            || policies < 0 || (policies & ~(1 << 3)) != 0
                            || result.put(admin, new Grant(owner, policies)) != null) return Map.of();
                }
                return in.read() == -1 ? result : Map.of();
            }
        } catch (IOException | IllegalArgumentException failure) { return Map.of(); }
    }
}
