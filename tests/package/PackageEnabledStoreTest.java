/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.Map;

/** The exact production store, real files and actual rejected writes; no storage mock. */
public final class PackageEnabledStoreTest {
    private static int checks;
    private static void check(boolean ok, String message) {
        if (!ok) throw new AssertionError(message); checks++;
    }
    private interface Action { void run() throws Exception; }
    private static void rejects(Action action, String message) throws Exception {
        try { action.run(); } catch (IllegalStateException expected) { checks++; return; }
        throw new AssertionError(message);
    }
    private static PackageEnabledStore.Entry entry(String owner, int app) {
        return new PackageEnabledStore.Entry(owner, new PackageEnabledStore.State(app, Map.of()));
    }
    private static void rejectedWrite(PackageEnabledStore store, Path file, String owner,
            Map<String, PackageEnabledStore.Entry> changes, String message) throws Exception {
        byte[] before = Files.readAllBytes(file);
        var state = store.state("fixture.live", owner);
        rejects(() -> store.commit(changes), message);
        check(Arrays.equals(before, Files.readAllBytes(file)), message + ": disk changed");
        check(store.state("fixture.live", owner) == state, message + ": memory published");
        check(new PackageEnabledStore(file.toFile(), file.resolveSibling("owners"))
                .state("fixture.live", owner).equals(state), message + ": old file no longer readable");
        try (var children = Files.list(file.getParent())) {
            check(children.noneMatch(p -> p.getFileName().toString().startsWith("enabled-state-")),
                    message + ": temporary file leaked");
        }
    }
    private static void corrupt(Path file, Path owners, byte[] bytes, String message) throws Exception {
        byte[] before = Files.readAllBytes(file); Files.write(file, bytes);
        try { rejects(() -> new PackageEnabledStore(file.toFile(), owners), message); }
        finally { Files.write(file, before); }
    }
    public static void main(String[] args) throws Exception {
        Path root = Path.of(args[0]); Files.createDirectories(root);
        Path file = root.resolve("state"), owners = root.resolve("owners");
        PackageEnabledStore store = new PackageEnabledStore(file.toFile(), owners);
        String live = store.owner("fixture.live"), old = store.owner("fixture.retired");
        check(live.equals(store.owner("fixture.live")), "Update changed installation owner");
        check(store.state("fixture.live", live).equals(PackageEnabledStore.State.DEFAULT), "New install was not DEFAULT");
        Map<String, Integer> input = new LinkedHashMap<>(); input.put("fixture.live.Alias", 1);
        var state = new PackageEnabledStore.State(3, input); input.clear();
        check(state.component("fixture.live.Alias") == 1, "State retained caller-owned map");
        try { state.components().clear(); throw new AssertionError("Mutable snapshot"); }
        catch (UnsupportedOperationException expected) { checks++; }
        store.commit(Map.of("fixture.live", new PackageEnabledStore.Entry(live, state),
                "fixture.retired", entry(old, 2)));
        check(new PackageEnabledStore(file.toFile(), owners).state("fixture.live", live).equals(state),
                "Restart lost application/component state");
        check(store.state("fixture.live", old).equals(PackageEnabledStore.State.DEFAULT), "Wrong owner inherited state");

        Map<String, PackageEnabledStore.Entry> tooMany = new LinkedHashMap<>();
        for (int i = 0; i < 20001; i++) tooMany.put("fixture.p" + i, entry(live, 1));
        rejectedWrite(store, file, live, tooMany, "Writer accepted unreadable package count");
        Map<String, Integer> components = new LinkedHashMap<>();
        for (int i = 0; i < 100001; i++) components.put("fixture.Component" + i, 1);
        rejectedWrite(store, file, live, Map.of("fixture.live", new PackageEnabledStore.Entry(live,
                new PackageEnabledStore.State(3, components))), "Writer accepted unreadable component count");
        components.clear();
        // Each UTF entry is legal; their combined serialized size exceeds 32 MiB.
        String longName = "x".repeat(60000);
        for (int i = 0; i < 560; i++) components.put("fixture.C" + i + longName, 1);
        rejectedWrite(store, file, live, Map.of("fixture.live", new PackageEnabledStore.Entry(live,
                new PackageEnabledStore.State(3, components))), "Writer accepted unreadable byte length");
        rejectedWrite(store, file, live, Map.of("fixture.live", new PackageEnabledStore.Entry(live,
                new PackageEnabledStore.State(3, Map.of("x".repeat(70000), 1)))), "Oversized UTF write succeeded");
        rejectedWrite(store, file, live, Map.of("fixture.live", entry("invalid-owner", 1)), "Invalid owner write succeeded");

        Path saved = root.resolve("saved"); Files.move(file, saved); Files.createDirectory(file);
        try {
            rejects(() -> store.commit(Map.of("fixture.live", entry(live, 1))), "Rename failure returned success");
            check(store.state("fixture.live", live).equals(state), "Rename failure published memory");
            check(Files.isDirectory(file) && Files.size(saved) > 0, "Rename failure destroyed old snapshot");
        } finally { Files.delete(file); Files.move(saved, file); }

        store.retire("fixture.retired", old);
        check(!Files.exists(owners.resolve("fixture.retired")), "Retirement retained owner");
        PackageEnabledStore restarted = new PackageEnabledStore(file.toFile(), owners);
        String replacement = restarted.owner("fixture.retired");
        check(!replacement.equals(old) && restarted.state("fixture.retired", replacement)
                .equals(PackageEnabledStore.State.DEFAULT), "Restart/reinstall revived an obsolete disk row");
        restarted.commit(Map.of("fixture.live", entry(live, 1)));
        try (var in = new DataInputStream(Files.newInputStream(file))) {
            in.readInt(); check(in.readInt() == 1, "Commit did not compact obsolete disk rows after restart");
        }

        corrupt(file, owners, new byte[]{1, 2, 3}, "Truncation silently re-enabled applications");
        byte[] trailing = Arrays.copyOf(Files.readAllBytes(file), (int)Files.size(file) + 1);
        corrupt(file, owners, trailing, "Trailing data was accepted");
        for (int count : new int[]{-1, 20001}) {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            try (DataOutputStream out = new DataOutputStream(bytes)) { out.writeInt(0x47455331); out.writeInt(count); }
            corrupt(file, owners, bytes.toByteArray(), "Invalid package count was accepted");
        }
        ByteArrayOutputStream duplicate = new ByteArrayOutputStream();
        try (DataOutputStream out = new DataOutputStream(duplicate)) {
            out.writeInt(0x47455331); out.writeInt(2);
            for (int i = 0; i < 2; i++) {
                out.writeUTF("fixture.live"); out.writeUTF(live); out.writeInt(1); out.writeInt(0);
            }
        }
        corrupt(file, owners, duplicate.toByteArray(), "Duplicate package was accepted");
        Path marker = owners.resolve("fixture.live"); byte[] markerBefore = Files.readAllBytes(marker);
        Files.writeString(marker, "0".repeat(36));
        try {
            rejects(() -> new PackageEnabledStore(file.toFile(), owners), "Malformed owner silently discarded disabled state");
            rejects(() -> restarted.owner("fixture.live"), "Malformed owner was replaced");
        } finally { Files.write(marker, markerBefore); }

        byte[] diskBefore = Files.readAllBytes(file), ownerBefore = Files.readAllBytes(marker);
        PackageEnabledStore transientStore = new PackageEnabledStore();
        String local = transientStore.owner("fixture.live");
        transientStore.commit(Map.of("fixture.live", entry(local, 3)));
        check(transientStore.state("fixture.live", local).application() == 3
                && transientStore.owner("fixture.live").equals(local), "Transient state lost local updates");
        check(Arrays.equals(diskBefore, Files.readAllBytes(file)) && Arrays.equals(ownerBefore, Files.readAllBytes(marker)),
                "Transient store wrote device policy");
        transientStore.retire("fixture.live", local);
        check(transientStore.state("fixture.live", transientStore.owner("fixture.live"))
                .equals(PackageEnabledStore.State.DEFAULT), "Transient reinstall inherited state");
        System.out.println("Package enabled store contracts passed: " + checks + " checks");
    }
}
