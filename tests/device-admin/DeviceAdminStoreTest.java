/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.nio.file.*;
import java.util.UUID;

public final class DeviceAdminStoreTest {
    private static void check(boolean condition, String detail) {
        if (!condition) throw new AssertionError(detail);
    }
    public static void main(String[] args) throws Exception {
        Path root = Path.of(args[0]), file = root.resolve("grants");
        String admin = "sample.app/.Admin", owner = UUID.randomUUID().toString();
        DeviceAdminStore store = new DeviceAdminStore(file);
        check(store.get(admin) == null, "fresh install is not active");
        var grant = new DeviceAdminStore.Grant(owner, 8);
        store.put(admin, grant);
        check(new DeviceAdminStore(file).get(admin).equals(grant), "grant did not survive process restart");
        check(!new DeviceAdminStore(file).get(admin).owner().equals(UUID.randomUUID().toString()),
                "reinstall must not inherit old install ownership");
        store.put(admin, null);
        check(new DeviceAdminStore(file).get(admin) == null, "revoked grant resurrected after restart");
        Files.write(file, new byte[]{1, 2, 3});
        check(new DeviceAdminStore(file).snapshot().isEmpty(), "truncated store granted authority");
        Files.write(file, new byte[]{0x47,0x44,0x41,0x31,0x7f,0x7f,0x7f,0x7f});
        check(new DeviceAdminStore(file).snapshot().isEmpty(), "unbounded record count accepted");
        DeviceAdminStore temporary = new DeviceAdminStore(null);
        temporary.put(admin, grant);
        check(new DeviceAdminStore(null).snapshot().isEmpty(), "standalone shared persistent policy");
        // A failed durability operation must not publish authority in memory.
        Path blocking = root.resolve("not-directory"); Files.writeString(blocking, "file");
        DeviceAdminStore failing = new DeviceAdminStore(blocking.resolve("grants"));
        try { failing.put(admin, grant); throw new AssertionError("I/O failure reported success"); }
        catch (IllegalStateException expected) { }
        check(failing.get(admin) == null, "I/O failure published a grant");
        System.out.println("DeviceAdminStoreTest passed");
    }
}
