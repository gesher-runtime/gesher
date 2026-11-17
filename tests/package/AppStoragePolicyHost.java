/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.pm.ApplicationInfo;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.PrintStream;
import java.lang.reflect.Field;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import sun.misc.Unsafe;

/** Production scan/storage ordering with real signed APK verification and filesystem operations. */
public final class AppStoragePolicyHost {
    private static int checks;
    private static Unsafe unsafe;
    private static Path signed, unsigned;

    private static void check(boolean condition, String reason) {
        if (!condition) throw new AssertionError(reason);
        checks++;
    }

    private static void set(Object object, String name, Object value) throws Exception {
        Field field = object.getClass().getDeclaredField(name);
        field.setAccessible(true); field.set(object, value);
    }

    private static ApkPackage record(Path directory, String name, boolean requestedDefault,
            Path source) throws Exception {
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(name, source.toString());
        parsed.setTargetSdkVersion(36);
        parsed.setDefaultToDeviceProtectedStorage(requestedDefault);
        parsed.hideAsFinal();
        ApkPackage apk = (ApkPackage) unsafe.allocateInstance(ApkPackage.class);
        set(apk, "packageName", name); set(apk, "parsed", parsed);
        set(apk, "apkPath", source.toString()); set(apk, "mUid", 10000);
        set(apk, "sharedLibraries", SharedLibraryGraph.EMPTY);
        apk.setDataDir(directory.toString());
        return apk;
    }

    private static ApkPackage system(Path directory, String name, boolean requestedDefault)
            throws Exception {
        ApkPackage apk = record(directory, name, requestedDefault, signed);
        check(!apk.isSystemAddon(), "Raw manifest started with system status");
        apk.setSystemAddon(true);
        check(apk.hasVerifiedSigningIdentity() && apk.isSystemAddon(),
                "Signed system-scan input failed real signature verification");
        return apk;
    }

    private static Path ce(ApkPackage apk) { return Path.of(apk.credentialProtectedDataDir()); }
    private static Path de(ApkPackage apk) { return Path.of(apk.deviceProtectedDataDir()); }
    private static Path journal(ApkPackage apk) {
        return AppDataStorage.custom(apk.credentialProtectedDataDir()).metadata.resolve("state.properties");
    }
    private static void write(Path path, String content) throws IOException {
        Files.createDirectories(path.getParent()); Files.writeString(path, content);
    }
    private static boolean contains(Path path, String content) throws IOException {
        return Files.exists(path) && content.equals(Files.readString(path));
    }
    private static List<ApkPackage> selected(ApkPackage apk) {
        return new ArrayList<>(List.of(apk));
    }
    private static void policy(ApkPackage apk, boolean expected) throws Exception {
        ApplicationInfo info = apk.applicationInfo(0);
        check(Path.of(info.dataDir).equals(expected ? de(apk) : ce(apk)),
                "Effective default data directory disagrees with Android system scan policy");
        check(((info.privateFlags & ApplicationInfo.PRIVATE_FLAG_DEFAULT_TO_DEVICE_PROTECTED_STORAGE)
                        != 0) == expected,
                "Published default flag disagrees with effective storage policy");
    }

    private static void ordinary(Path root) throws Exception {
        for (boolean requestedDefault : new boolean[] {false, true}) {
            String name = "fixture.policy.ordinary" + requestedDefault;
            ApkPackage apk = record(root.resolve(name), name, requestedDefault, unsigned);
            write(ce(apk).resolve("files/keep"), "credential");
            try {
                apk.setSystemAddon(true);
                throw new AssertionError("Unsigned APK acquired system storage policy");
            } catch (SecurityException expected) {
                check(!apk.isSystemAddon(), "Rejected promotion changed system status");
            }
            SystemServer.preparePackageStorage(List.of(apk), selected(apk));
            policy(apk, false);
            check(contains(ce(apk).resolve("files/keep"), "credential")
                            && !Files.exists(de(apk).resolve("files/keep")),
                    "Ordinary manifest default moved credential data");
            check(Files.isDirectory(de(apk)), "Ordinary app has no explicit DE context directory");
        }
    }

    private static void classifiedBeforePreparation(Path root) throws Exception {
        String name = "fixture.policy.system";
        ApkPackage apk = record(root.resolve(name), name, true, signed);
        write(ce(apk).resolve("files/old"), "old default");
        policy(apk, false);
        check(!Files.exists(journal(apk)), "Metadata query finalized policy before system scan");
        apk.setSystemAddon(true); // Uses the actual signed fixture, not injected verification state.
        SystemServer.preparePackageStorage(List.of(apk), selected(apk));
        policy(apk, true);
        check(contains(de(apk).resolve("files/old"), "old default")
                        && !Files.exists(ce(apk).resolve("files/old")),
                "System classification was applied after legacy data had been finalized as CE");
        write(ce(apk).resolve("files/new-ce"), "independent credential");
        ApkPackage restart = system(ce(apk), name, true);
        SystemServer.preparePackageStorage(List.of(restart), selected(restart));
        check(contains(ce(restart).resolve("files/new-ce"), "independent credential")
                        && contains(de(restart).resolve("files/old"), "old default")
                        && !Files.exists(de(restart).resolve("files/new-ce")),
                "Restart repeated migration or lost independent data");
    }

    private static void committedAuthority(Path root) throws Exception {
        String name = "fixture.policy.newdefault";
        ApkPackage previous = system(root.resolve(name), name, false);
        ApkPackage incoming = system(ce(previous), name, true);
        write(ce(previous).resolve("files/old"), "previous CE");
        SystemServer.preparePackageStorage(List.of(previous), selected(incoming));
        policy(incoming, true);
        check(contains(ce(previous).resolve("files/old"), "previous CE")
                        && !Files.exists(de(incoming).resolve("files/old")),
                "Incoming default authorized migration of committed CE data");

        name = "fixture.policy.fallback";
        previous = system(root.resolve(name), name, true);
        ApkPackage fallback = system(ce(previous), name, false);
        write(ce(previous).resolve("files/old"), "previous DE");
        SystemServer.preparePackageStorage(List.of(previous), selected(fallback));
        policy(fallback, false);
        check(contains(de(previous).resolve("files/old"), "previous DE")
                        && !Files.exists(ce(fallback).resolve("files/old")),
                "Selected fallback replaced the committed manifest's migration authority");

        name = "fixture.policy.uncommitted";
        ApkPackage bare = system(root.resolve(name), name, true);
        write(ce(bare).resolve("files/old"), "unclassified");
        try {
            SystemServer.preparePackageStorage(List.of(), selected(bare));
            throw new AssertionError("Uncommitted APK authorized legacy migration");
        } catch (IOException expected) {
            check(contains(ce(bare).resolve("files/old"), "unclassified")
                            && !Files.exists(journal(bare)),
                    "Rejected bare APK changed legacy data or classified its layout");
        }
    }

    private static void recoveryAndFailure(Path root) throws Exception {
        String name = "fixture.policy.recovery";
        ApkPackage apk = system(root.resolve(name), name, true);
        write(ce(apk).resolve("files/old"), "recoverable");
        write(journal(apk), "version=1\nphase=moving-default-de\n");
        SystemServer.preparePackageStorage(List.of(apk), selected(apk));
        check(contains(de(apk).resolve("files/old"), "recoverable")
                        && Files.isDirectory(ce(apk)),
                "Committed system migration did not recover before publication");

        name = "fixture.policy.collision";
        ApkPackage previous = system(root.resolve(name), name, true);
        ApkPackage replacement = system(ce(previous), name, false);
        write(ce(previous).resolve("files/old"), "old data");
        write(de(previous).resolve("files/other"), "destination data");
        List<ApkPackage> selected = selected(replacement);
        SystemServer.preparePackageStorage(List.of(previous), selected);
        check(selected.isEmpty(), "Failed committed preparation still published an incoming APK");
        check(contains(ce(previous).resolve("files/old"), "old data")
                        && contains(de(previous).resolve("files/other"), "destination data")
                        && !Files.exists(journal(previous)),
                "Failed preparation overwrote data or finalized the wrong default policy");
    }

    private static void commandLineSelection(Path root) throws Exception {
        String name = "fixture.policy.argv";
        ApkPackage previous = record(root.resolve(name), name, false, unsigned);
        write(ce(previous).resolve("files/old"), "committed CE");
        List<ApkPackage> selected = selected(previous);
        for (boolean requested : new boolean[] {true, false, true}) {
            ApkPackage incoming = record(ce(previous), name, requested, unsigned);
            SystemServer.selectCommandLinePackage(selected, incoming);
            check(selected.size() == 1 && selected.get(0) == incoming && !Files.exists(journal(previous)),
                    "Explicit APK selection prepared storage before the committed policy was applied");
        }
        SystemServer.preparePackageStorage(List.of(previous), selected);
        check(selected.size() == 1 && contains(ce(previous).resolve("files/old"), "committed CE")
                        && !Files.exists(de(previous).resolve("files/old")),
                "Repeated explicit APKs replaced committed storage policy");

        name = "fixture.policy.argvblocked";
        ApkPackage system = system(root.resolve(name), name, true);
        write(ce(system).resolve("files/old"), "unclassified legacy");
        write(de(system).resolve("files/other"), "existing destination");
        selected = selected(system);
        // Merely having a signature is not authorization to replace a system slot.
        // Selection must still see the system predecessor until all inputs are resolved.
        for (Path source : new Path[] {unsigned, signed}) {
            ApkPackage incoming = record(ce(system), name, false, source);
            try {
                SystemServer.selectCommandLinePackage(selected, incoming);
                throw new AssertionError("Explicit APK lost its predecessor's system trust policy");
            } catch (SecurityException expected) {
                check(selected.size() == 1 && selected.get(0) == system
                                && system.isSystemAddon() && !Files.exists(journal(system)),
                        "Rejected explicit APK changed system identity or finalized ambiguous storage");
            }
        }
        SystemServer.preparePackageStorage(List.of(system), selected);
        check(selected.isEmpty() && !Files.exists(journal(system))
                        && contains(ce(system).resolve("files/old"), "unclassified legacy")
                        && contains(de(system).resolve("files/other"), "existing destination"),
                "Repeated explicit APK bypassed exclusion of an ambiguous committed layout");
    }

    /** Capture the actual production preparation boundary, including observation at log time. */
    private static String preparationOutput(List<ApkPackage> committed, List<ApkPackage> selected,
            boolean expectFailure) throws Exception {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        PrintStream original = System.out;
        try (PrintStream capture = new PrintStream(output, true, StandardCharsets.UTF_8) {
            @Override public void println(String line) {
                if (line.contains(" parsed by PackageParser2: ")) {
                    for (ApkPackage apk : selected) {
                        check(Files.isDirectory(ce(apk)) && Files.isDirectory(de(apk)),
                                "Parser summary preceded preparation of the final selection");
                        try {
                            check(Files.readString(journal(apk)).contains("phase=ready"),
                                    "Parser summary preceded storage journal completion");
                        } catch (IOException failure) {
                            throw new AssertionError("Parser summary has no completed layout", failure);
                        }
                    }
                }
                super.println(line);
            }
        }) {
            System.setOut(capture);
            try {
                SystemServer.preparePackageStorage(committed, selected);
                check(!expectFailure, "Invalid selected storage unexpectedly prepared");
            } catch (IOException failure) {
                check(expectFailure, "Valid selected storage could not be prepared: " + failure);
            }
        } finally {
            System.setOut(original);
        }
        return output.toString(StandardCharsets.UTF_8);
    }

    private static void startupDiagnostics(Path root) throws Exception {
        ApkPackage ordinary = record(root.resolve("diagnostic-ordinary"),
                "fixture.policy.diagnosticordinary", true, signed);
        // Signature verification alone is not a system scan or a default-DE grant.
        check(ordinary.hasVerifiedSigningIdentity() && !ordinary.isSystemAddon(),
                "Signed ordinary diagnostic input acquired system classification");
        ApkPackage trusted = system(root.resolve("diagnostic-system"),
                "fixture.policy.diagnosticsystem", true);
        List<ApkPackage> selected = new ArrayList<>(List.of(ordinary, trusted));
        String output = preparationOutput(List.of(), selected, false);
        check(output.lines().toList().equals(List.of("system_server: " + ordinary.describe(),
                        "system_server: " + trusted.describe())),
                "REGRESSION: system_server parser diagnostics lost their documented format or final-selection order");
        policy(ordinary, false); policy(trusted, true);

        ApkPackage earlier = record(root.resolve("diagnostic-earlier"),
                "fixture.policy.diagnosticearlier", false, unsigned);
        ApkPackage blocked = record(root.resolve("diagnostic-blocked"),
                "fixture.policy.diagnosticblocked", false, unsigned);
        write(ce(blocked), "blocked directory");
        output = preparationOutput(List.of(), new ArrayList<>(List.of(earlier, blocked)), true);
        check(Files.isDirectory(ce(earlier)) && !output.contains(" parsed by PackageParser2: "),
                "Late preparation failure left a successful parser summary");

        ApkPackage collision = system(root.resolve("diagnostic-collision"),
                "fixture.policy.diagnosticcollision", true);
        write(ce(collision).resolve("keep"), "old");
        write(de(collision).resolve("keep"), "occupied");
        selected = new ArrayList<>(List.of(collision, ordinary));
        output = preparationOutput(List.of(collision), selected, false);
        check(selected.equals(List.of(ordinary))
                        && output.contains("storage could not be prepared:")
                        && output.lines().filter(line -> line.contains(" parsed by PackageParser2: "))
                                .toList().equals(List.of("system_server: " + ordinary.describe())),
                "Excluded committed package received a successful parser summary");
    }

    private static void splitFixturePolicy(Path root) throws Exception {
        ApkPackage apk = record(root.resolve("split-policy"), "fixture.policy.split", true, unsigned);
        PackageImpl parsed = (PackageImpl) apk.parsed;
        parsed.setDebuggable(true); parsed.setLargeHeap(true);
        parsed.setCleartextTrafficAllowed(true); parsed.setHardwareAccelerated(true);
        parsed.setExtractNativeLibrariesRequested(true); parsed.setRtlSupported(true);
        parsed.setBackupAllowed(false); parsed.setDeclaredHavingCode(true);
        parsed.setDirectBootAware(true); parsed.setIsolatedSplitLoading(true);
        parsed.setResizeableActivity(false); parsed.hideAsFinal();
        set(apk, "mHasCode", true);
        // Execute the same policy assertions used after real split install/reload on ART.
        // Native parsing, split DEX and resources remain the Windows fixture's coverage.
        gesher.services.SplitPackageContractTest.ordinaryApplicationPolicy(apk, apk.applicationInfo(0));
        check(!Files.exists(journal(apk)), "Split metadata policy check prepared app storage");
        System.out.println("SplitPackageContractTest ordinary policy assertions passed on host");
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Field singleton = Unsafe.class.getDeclaredField("theUnsafe");
        singleton.setAccessible(true); unsafe = (Unsafe) singleton.get(null);
        signed = Path.of(args[1], "v2only.apk"); unsigned = Path.of(args[1], "unsigned.apk");
        Path root = Files.createTempDirectory("gesher-storage-policy-");
        try {
            ordinary(root); classifiedBeforePreparation(root);
            committedAuthority(root); recoveryAndFailure(root);
            commandLineSelection(root);
            startupDiagnostics(root); splitFixturePolicy(root);
            gesher.services.PackageManifestFlagsTest.main(new String[0]);
            System.out.println("AppStoragePolicyHost all passed: " + checks + " checks");
        } finally {
            try (var paths = Files.walk(root)) {
                for (Path path : paths.sorted(java.util.Comparator.reverseOrder()).toList())
                    Files.delete(path);
            }
        }
    }
}
