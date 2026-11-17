/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.io.IOException;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

/** Real production layout, journal and uninstall code against real filesystem operations. */
public final class AppDataStorageHostTest {
    private static int checks;
    private static final String PACKAGE = "fixture.storage.owner";

    private static void check(boolean value, String why) {
        if (!value) throw new AssertionError(why);
        checks++;
    }

    private interface Call { void run() throws Exception; }
    private static void rejects(Call call, String why) throws Exception {
        try { call.run(); } catch (IOException expected) { checks++; return; }
        throw new AssertionError(why);
    }

    private static void write(Path file, String text) throws IOException {
        Files.createDirectories(file.getParent());
        Files.writeString(file, text);
    }

    private static void equal(Path file, String expected, String why) throws IOException {
        check(Files.isRegularFile(file) && expected.equals(Files.readString(file)), why);
    }

    private static void phase(AppDataStorage storage, String value) throws IOException {
        write(storage.metadata.resolve("state.properties"), "version=1\nphase=" + value + "\n");
    }

    private static AppDataStorage layout(Path root, String name) {
        return AppDataStorage.installed(root.resolve(name).toFile(), PACKAGE);
    }

    private static void layoutAndMigration(Path root) throws Exception {
        AppDataStorage ordinary = layout(root, "ordinary");
        check(!Files.exists(ordinary.credential) && !Files.exists(ordinary.metadata),
                "metadata construction created application storage");
        check(ordinary.credential.equals(root.resolve("ordinary/data/" + PACKAGE))
                        && ordinary.device.equals(root.resolve("ordinary/user_de/0/" + PACKAGE)),
                "installed CE/DE paths do not match the explicit disk contract");
        write(ordinary.credential.resolve("files/same"), "credential");
        ordinary.prepare(false, true);
        write(ordinary.device.resolve("files/same"), "device");
        equal(ordinary.credential.resolve("files/same"), "credential", "DE overwrote CE filename");
        equal(ordinary.device.resolve("files/same"), "device", "CE and DE alias each other");
        // A later APK changing its manifest default must not move existing CE data.
        ordinary.prepare(true, false);
        equal(ordinary.credential.resolve("files/same"), "credential", "manifest update migrated CE data");
        equal(ordinary.device.resolve("files/same"), "device", "manifest update overwrote DE data");

        AppDataStorage legacy = layout(root, "legacy-default-de");
        write(legacy.credential.resolve("databases/application.db"), "old default database");
        write(legacy.credential.resolve("shared_prefs/settings.xml"), "old default preferences");
        legacy.prepare(true, true);
        equal(legacy.device.resolve("databases/application.db"), "old default database", "legacy database stranded");
        equal(legacy.device.resolve("shared_prefs/settings.xml"), "old default preferences", "legacy preferences stranded");
        check(Files.isDirectory(legacy.credential)
                        && !Files.exists(legacy.credential.resolve("databases/application.db")),
                "legacy default migration copied both stores or omitted the separate CE area");
        write(legacy.credential.resolve("files/new-ce"), "separate after migration");
        legacy.prepare(true, true);
        legacy.prepare(false, false);
        equal(legacy.credential.resolve("files/new-ce"), "separate after migration", "restart reran migration");
        check(!Files.exists(legacy.device.resolve("files/new-ce")), "restart leaked new CE data into DE");

        AppDataStorage incoming = layout(root, "incoming-default-change");
        write(incoming.credential.resolve("files/kept"), "original CE");
        rejects(() -> incoming.prepare(true, false), "incoming APK authorized legacy migration");
        equal(incoming.credential.resolve("files/kept"), "original CE", "failed install changed old data");
        check(!Files.exists(incoming.device)
                        && !Files.exists(incoming.metadata.resolve("state.properties")),
                "rejected migration published a new layout");
        // Reading the old committed CE-default manifest establishes the old layout;
        // the exact same incoming default-DE metadata can now be used without moving it.
        incoming.prepare(false, true);
        incoming.prepare(true, false);
        equal(incoming.credential.resolve("files/kept"), "original CE", "new default silently moved old CE");
        check(!Files.exists(incoming.device.resolve("files/kept")), "new default copied legacy CE");

        AppDataStorage unpreparedUpdate = layout(root, "unprepared-update");
        write(unpreparedUpdate.credential.resolve("old-default"), "legacy default data");
        for (boolean incomingDefault : new boolean[] {false, true}) {
            rejects(() -> unpreparedUpdate.prepareUpdate(incomingDefault),
                    "incoming manifest initialized an existing app's legacy layout");
            check(!Files.exists(unpreparedUpdate.metadata.resolve("state.properties")),
                    "failed update permanently classified the old layout");
            equal(unpreparedUpdate.credential.resolve("old-default"), "legacy default data",
                    "failed update changed unprepared old data");
        }
        unpreparedUpdate.prepare(true, true); // old committed DE-default manifest
        unpreparedUpdate.prepareUpdate(false); // now a later default change is legitimate
        equal(unpreparedUpdate.device.resolve("old-default"), "legacy default data",
                "later update changed the original layout identity");

        AppDataStorage custom = AppDataStorage.custom(root.resolve("custom-data").toString());
        check(custom.credential.equals(root.resolve("custom-data"))
                        && custom.device.equals(root.resolve("custom-data.device-protected"))
                        && !custom.device.startsWith(custom.credential),
                "--data-dir override aliases or nests its storage areas");
        custom.prepare(false, false);
        check(Files.isDirectory(custom.credential) && Files.isDirectory(custom.device),
                "explicit launch path did not prepare both areas");
    }

    private static void interruptedMigration(Path root) throws Exception {
        AppDataStorage before = layout(root, "crash-before-rename");
        write(before.credential.resolve("files/data"), "before");
        phase(before, "moving-default-de");
        before.prepare(true, false); // the already durable journal is the authority
        equal(before.device.resolve("files/data"), "before", "pre-rename recovery lost data");

        AppDataStorage after = layout(root, "crash-after-rename");
        write(after.device.resolve("files/data"), "after");
        phase(after, "moving-default-de");
        after.prepare(false, false);
        equal(after.device.resolve("files/data"), "after", "post-rename recovery lost data");
        check(Files.isDirectory(after.credential), "post-rename recovery omitted empty CE");

        AppDataStorage conflict = layout(root, "ambiguous-both");
        write(conflict.credential.resolve("value"), "source");
        write(conflict.device.resolve("value"), "target");
        phase(conflict, "moving-default-de");
        rejects(() -> conflict.prepare(true, true), "ambiguous migration merged two trees");
        equal(conflict.credential.resolve("value"), "source", "conflict removed CE");
        equal(conflict.device.resolve("value"), "target", "conflict replaced DE");

        AppDataStorage collision = layout(root, "initial-collision");
        write(collision.credential.resolve("value"), "source");
        Files.createDirectories(collision.device);
        rejects(() -> collision.prepare(true, true), "legacy migration replaced an existing DE directory");
        check(!Files.exists(collision.metadata.resolve("state.properties")), "collision published migration state");

        AppDataStorage missing = layout(root, "missing-both");
        phase(missing, "moving-default-de");
        rejects(() -> missing.prepare(true, true), "missing migrated data was silently accepted");
        AppDataStorage unknown = layout(root, "unknown-version");
        write(unknown.metadata.resolve("state.properties"), "version=999\nphase=ready\n");
        rejects(() -> unknown.prepare(false, false), "unknown storage version was overwritten");
        AppDataStorage obstruction = layout(root, "obstructed");
        write(obstruction.device, "not a directory");
        rejects(() -> obstruction.prepare(false, false), "obstructed DE path was accepted");

        AppDataStorage linked = layout(root, "linked");
        Path outside = root.resolve("unrelated-data");
        write(outside.resolve("sentinel"), "unrelated");
        Files.createDirectories(linked.device.getParent());
        Files.createSymbolicLink(linked.device, outside);
        rejects(() -> linked.prepare(false, false), "DE symbolic link was followed");
        equal(outside.resolve("sentinel"), "unrelated", "link rejection touched unrelated data");
    }

    private static void concurrentPreparation(Path root) throws Exception {
        Path data = root.resolve("concurrent");
        AppDataStorage storage = AppDataStorage.installed(data.toFile(), PACKAGE);
        write(storage.credential.resolve("files/original"), "one original");
        List<Process> children = new ArrayList<>();
        for (int i = 0; i < 4; i++) {
            children.add(new ProcessBuilder(Path.of(System.getProperty("java.home"), "bin/java").toString(),
                    "-cp", System.getProperty("java.class.path"), AppDataStorageHostTest.class.getName(),
                    "prepare", data.toString()).inheritIO().start());
        }
        for (Process child : children) {
            if (!child.waitFor(20, TimeUnit.SECONDS)) {
                child.destroyForcibly(); throw new AssertionError("Concurrent preparation deadlocked");
            }
            check(child.exitValue() == 0, "concurrent preparer failed");
        }
        equal(storage.device.resolve("files/original"), "one original", "concurrent migration lost data");
        check(!Files.exists(storage.credential.resolve("files/original")), "concurrent migration duplicated data");

        AppDataStorage blocked = layout(root, "interrupted-lock");
        Files.createDirectories(blocked.metadata);
        try (FileChannel held = FileChannel.open(blocked.metadata.resolve("lock"),
                StandardOpenOption.CREATE, StandardOpenOption.WRITE);
                FileLock lock = held.tryLock()) {
            check(lock != null, "fixture failed to own its layout lock");
            CountDownLatch entered = new CountDownLatch(1);
            AtomicReference<Throwable> outcome = new AtomicReference<>();
            Thread waiting = new Thread(() -> {
                entered.countDown();
                try {
                    blocked.prepare(false, false);
                    outcome.set(new AssertionError("Contended layout was prepared without ownership"));
                } catch (IOException interrupted) {
                    if (!Thread.currentThread().isInterrupted()) {
                        outcome.set(new AssertionError("Lost interruption status", interrupted));
                    }
                } catch (Throwable error) { outcome.set(error); }
            });
            waiting.start();
            check(entered.await(5, TimeUnit.SECONDS), "lock waiter did not start");
            Thread.sleep(50);
            waiting.interrupt(); waiting.join(5000);
            check(!waiting.isAlive() && outcome.get() == null, "interrupted lock waiter failed: " + outcome.get());
            check(!Files.exists(blocked.metadata.resolve("state.properties")),
                    "interrupted contender published layout state");
        }
        blocked.prepare(false, false);
        check(Files.isDirectory(blocked.credential) && Files.isDirectory(blocked.device),
                "released layout lock could not be reacquired");
    }

    private static void removalRecovery(Path root) throws Exception {
        Path data = root.resolve("removal");
        System.setProperty("gesher.test.data", data.toString());
        AppDataStorage storage = AppDataStorage.installed(data.toFile(), PACKAGE);
        Path code = data.resolve("app/" + PACKAGE + "-1");
        write(code.resolve("base.apk"), "code");
        storage.prepare(false, false);
        write(storage.credential.resolve("same"), "CE");
        write(storage.device.resolve("same"), "DE");
        for (String kind : new String[] {"data", "obb", "media"}) {
            write(data.resolve("media/0/Android/" + kind + "/" + PACKAGE + "/same"), kind);
        }
        PackageRemovalStorage pending = new PackageRemovalStorage(PACKAGE, code.toString());
        pending.stage();
        check(!Files.exists(storage.credential) && !Files.exists(storage.device)
                        && !Files.exists(storage.metadata), "uninstall retained an original private data area");
        PackageRemovalStorage.recover(Map.of(PACKAGE, code.toString()));
        equal(storage.credential.resolve("same"), "CE", "uncommitted uninstall failed to restore CE");
        equal(storage.device.resolve("same"), "DE", "uncommitted uninstall failed to restore DE");
        check(Files.isRegularFile(storage.metadata.resolve("state.properties")), "rollback lost layout identity");
        for (String kind : new String[] {"data", "obb", "media"}) {
            equal(data.resolve("media/0/Android/" + kind + "/" + PACKAGE + "/same"), kind,
                    "named storage slots changed legacy external quarantine numbering");
        }

        PackageRemovalStorage committed = new PackageRemovalStorage(PACKAGE, code.toString());
        committed.stage();
        PackageRemovalStorage.recover(Collections.emptyMap());
        storage.prepare(false, false);
        check(!Files.exists(storage.credential.resolve("same"))
                        && !Files.exists(storage.device.resolve("same")),
                "reinstall inherited removed CE or DE data");

        // A journal from before CE/DE support has only fixed item-0..4 entries.
        Path oldCode = data.resolve("app/" + PACKAGE + "-2");
        Path trash = data.resolve("system/uninstall-trash/uninstall-old-format");
        write(trash.resolve("journal"), "package=" + PACKAGE + "\ncode=app/" + PACKAGE + "-2\n");
        write(trash.resolve("item-0/base.apk"), "legacy code");
        write(trash.resolve("item-1/old"), "legacy CE");
        PackageRemovalStorage.deleteNoFollow(storage.credential);
        for (int i = 0; i < 3; i++) {
            String kind = new String[] {"data", "obb", "media"}[i];
            write(trash.resolve("item-" + (i + 2) + "/old"), kind);
            PackageRemovalStorage.deleteNoFollow(data.resolve("media/0/Android/" + kind + "/" + PACKAGE));
        }
        write(storage.device.resolve("fresh"), "fresh DE is independent");
        PackageRemovalStorage.recover(Map.of(PACKAGE, oldCode.toString()));
        equal(oldCode.resolve("base.apk"), "legacy code", "old journal code changed slots");
        equal(storage.credential.resolve("old"), "legacy CE", "old journal CE changed slots");
        equal(storage.device.resolve("fresh"), "fresh DE is independent", "old journal modified new DE");
        for (String kind : new String[] {"data", "obb", "media"}) {
            equal(data.resolve("media/0/Android/" + kind + "/" + PACKAGE + "/old"), kind,
                    "old journal restored external data into wrong slot");
        }
    }

    private static void removalAliases(Path root) throws Exception {
        Path data = root.resolve("removal-alias"), alias = root.resolve("removal-alias-spelling");
        Path code = data.resolve("app/" + PACKAGE + "-1");
        write(code.resolve("base.apk"), "installed code");
        System.setProperty("gesher.test.data", data.toString());
        Files.createSymbolicLink(alias, data);
        PackageRemovalStorage pending = new PackageRemovalStorage(PACKAGE,
                alias.resolve("app/" + PACKAGE + "-1").toString());
        pending.stage();
        check(!Files.exists(code), "aliased uninstall retained installed code");
        pending.rollback();
        equal(code.resolve("base.apk"), "installed code", "aliased rollback lost installed code");
        String aliasedCode = alias.resolve("app/" + PACKAGE + "-1").toString();
        pending = new PackageRemovalStorage(PACKAGE, aliasedCode);
        pending.stage();
        check(PackageRemovalStorage.reserves(Path.of(aliasedCode)), "aliased generation lost its crash reservation");
        check(PackageRemovalStorage.reserves(code), "canonical generation lost its crash reservation");
        System.setProperty("gesher.test.data", alias.toString());
        PackageRemovalStorage.recover(Map.of(PACKAGE, aliasedCode));
        equal(code.resolve("base.apk"), "installed code", "recovery discarded an aliased retained installation");
        check(!PackageRemovalStorage.reserves(Path.of(aliasedCode)), "completed recovery retained its generation reservation");
        System.setProperty("gesher.test.data", data.toString());

        Path outside = root.resolve("removal-outside");
        write(outside.resolve("sentinel"), "unrelated");
        Path leaf = data.resolve("app/" + PACKAGE + "-2");
        Files.createSymbolicLink(leaf, outside);
        pending = new PackageRemovalStorage(PACKAGE, alias.resolve("app/" + PACKAGE + "-2").toString());
        pending.stage(); pending.discard();
        check(!Files.exists(leaf, java.nio.file.LinkOption.NOFOLLOW_LINKS), "APK leaf link was not quarantined");
        equal(outside.resolve("sentinel"), "unrelated", "APK leaf link traversal deleted unrelated data");

        Path escaped = data.resolve("outside-app");
        Files.createSymbolicLink(escaped, outside);
        try {
            new PackageRemovalStorage(PACKAGE, escaped.resolve(PACKAGE + "-3").toString());
            throw new AssertionError("outside APK parent was admitted");
        } catch (SecurityException expected) { checks++; }
        equal(outside.resolve("sentinel"), "unrelated", "parent rejection modified unrelated data");
    }

    private static int probeFromAnotherProcess(Path path) throws Exception {
        Process process = new ProcessBuilder(Path.of(System.getProperty("java.home"), "bin/java").toString(),
                "-cp", System.getProperty("java.class.path"), AppDataStorageHostTest.class.getName(),
                "probe-lock", path.toString()).redirectErrorStream(true).start();
        if (!process.waitFor(10, TimeUnit.SECONDS)) {
            process.destroyForcibly(); throw new AssertionError("External lock probe hung");
        }
        String output = new String(process.getInputStream().readAllBytes(), java.nio.charset.StandardCharsets.UTF_8);
        if (process.exitValue() != 0 && process.exitValue() != 3) {
            throw new AssertionError("External lock probe failed: " + output);
        }
        return process.exitValue(); // 0: excluded, 3: acquired and released
    }

    private static void awaitLockWait(Thread thread) throws Exception {
        long started = System.nanoTime();
        do {
            Thread.State state = thread.getState();
            if (state == Thread.State.WAITING || state == Thread.State.TIMED_WAITING) {
                for (StackTraceElement frame : thread.getStackTrace()) {
                    if (frame.getClassName().equals(AppDataStorage.class.getName())
                            && (frame.getMethodName().equals("awaitLocalGuard")
                                || frame.getMethodName().equals("awaitLock"))) return;
                }
            }
            if (!thread.isAlive()) throw new AssertionError("Contender ended before waiting for the lock");
            Thread.sleep(1);
        } while (System.nanoTime() - started < TimeUnit.SECONDS.toNanos(5));
        throw new AssertionError("Contender never entered the production lock wait");
    }

    /** Closing a canceled waiter's descriptor must not release the owner's process lock. */
    private static void canceledContenderKeepsOwnerLock(Path root) throws Exception {
        Path data = root.resolve("owner-lock");
        Files.createDirectories(data);
        Path alias = root.resolve("owner-lock-alias");
        Files.createSymbolicLink(alias, data);
        AppDataStorage owner = AppDataStorage.installed(data.toFile(), PACKAGE);
        AppDataStorage same = AppDataStorage.installed(data.toFile(), PACKAGE);
        AppDataStorage otherSpelling = AppDataStorage.installed(alias.toFile(), PACKAGE);
        // Acquire the exact production scope used by prepare, including its
        // in-process guard. A raw FileChannel holder would bypass the contract
        // under test and could not reveal whether production owners are protected.
        Method lockStorage = AppDataStorage.class.getDeclaredMethod("lockStorage");
        lockStorage.setAccessible(true);
        try (AutoCloseable held = (AutoCloseable) lockStorage.invoke(owner)) {
            check(probeFromAnotherProcess(owner.metadata.resolve("lock")) == 0,
                    "production owner did not exclude another process");
            for (AppDataStorage contender : new AppDataStorage[] {same, otherSpelling}) {
                AtomicReference<Throwable> outcome = new AtomicReference<>();
                Thread waiter = new Thread(() -> {
                    try {
                        contender.prepare(false, false);
                        outcome.set(new AssertionError("Contender entered an owned layout"));
                    } catch (IOException expected) {
                        if (!Thread.currentThread().isInterrupted()) {
                            outcome.set(new AssertionError("Contender lost interruption status", expected));
                        }
                    } catch (Throwable failure) { outcome.set(failure); }
                }, "storage-canceled-contender");
                waiter.start();
                try {
                    awaitLockWait(waiter);
                } finally {
                    waiter.interrupt(); waiter.join(5000);
                }
                check(!waiter.isAlive() && outcome.get() == null,
                        "contender failed to cancel: " + outcome.get());
                check(!Files.exists(owner.metadata.resolve("state.properties")),
                        "canceled contender published layout state");
                check(probeFromAnotherProcess(owner.metadata.resolve("lock")) == 0,
                        "REGRESSION: canceling an in-process contender released the production owner's file lock");
            }
            rejects(() -> owner.prepare(false, false), "recursive preparation opened another lock descriptor");
            check(probeFromAnotherProcess(owner.metadata.resolve("lock")) == 0,
                    "recursive preparation released the outer owner's file lock");
        }
        check(probeFromAnotherProcess(owner.metadata.resolve("lock")) == 3,
                "finished production owner retained its file lock");
        otherSpelling.prepare(false, false);
        check(Files.isDirectory(owner.credential) && Files.isDirectory(owner.device),
                "alias could not prepare storage after ownership ended");
        Field localLocks = AppDataStorage.class.getDeclaredField("sLocalLocks");
        localLocks.setAccessible(true);
        Map<?, ?> registry = (Map<?, ?>) localLocks.get(null);
        synchronized (registry) {
            check(registry.isEmpty(), "completed/canceled layout guards leaked registry entries");
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 2 && "probe-lock".equals(args[0])) {
            boolean acquired;
            try (FileChannel channel = FileChannel.open(Path.of(args[1]), StandardOpenOption.WRITE);
                    FileLock lock = channel.tryLock()) {
                acquired = lock != null;
            }
            System.exit(acquired ? 3 : 0);
            return;
        }
        if (args.length == 2 && "prepare".equals(args[0])) {
            AppDataStorage.installed(Path.of(args[1]).toFile(), PACKAGE).prepare(true, true);
            return;
        }
        Path root = Files.createTempDirectory("gesher-data-storage-").toAbsolutePath();
        try {
            layoutAndMigration(root);
            interruptedMigration(root);
            concurrentPreparation(root);
            removalRecovery(root);
            removalAliases(root);
            canceledContenderKeepsOwnerLock(root);
            System.out.println("AppDataStorageHostTest all passed: " + checks + " checks");
        } finally {
            PackageRemovalStorage.deleteNoFollow(root);
        }
    }
}
