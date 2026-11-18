/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import java.lang.ref.WeakReference;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Real JVM weak identities exercising the production Gesher namespace adapter.
 * This is a HotSpot JNI host test, not ART/JNI_OnLoad or Windows execution. */
public final class NativeNamespaceRegistryTest {
    private static native String create(ClassLoader loader, int sdk, boolean shared,
            String paths, String permitted, String uses);
    private static native long open(ClassLoader loader, int sdk, String path, String paths);
    private static native int value(long handle);
    private static native String[][] builtins();
    private static native String[][] publicFiles();
    private static native long loadedCount();
    private static native long publicLinkCount();
    private static native long processRssKiB();
    private static native void setApplicationTargetSdk(int version);
    private static native int applicationTargetSdk();
    private static native String raceCreate(ClassLoader loader);
    private static native void awaitCreationAttempt();
    private static native void awaitCreationSnapshot();
    private static native void resumeCreation();
    private static int checks, callbacks;
    private static final ClassLoader callbackLoader = new ClassLoader(null) {};
    private static boolean race;
    private static Thread creator;
    private static volatile String creationError;

    private static ClassLoader loader(ClassLoader parent) { return new ClassLoader(parent) {}; }
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        ++checks;
        System.out.println("PASS native namespace registry: " + message);
    }
    private static void create(ClassLoader loader, int sdk, boolean shared, String path, String uses) {
        String result = create(loader, sdk, shared, path, path, uses);
        check(result == null, "create namespace: " + (result == null ? "ok" : result));
    }
    private static void absent(ClassLoader loader, int sdk, String name) {
        try {
            open(loader, sdk, name, null);
        } catch (UnsatisfiedLinkError expected) {
            check(!expected.getMessage().isEmpty(), "ordinary load failure: " + name);
            return;
        }
        throw new AssertionError("Unexpected accessible library " + name);
    }
    // Invoked by an actual loaded ELF constructor through a registered fixture
    // provider. Nested creation and load would deadlock if a registry lock
    // crossed the constructor/callback boundary.
    public static void constructorCallback() {
        ++callbacks;
        if (callbacks == 1) create(callbackLoader, 31, false, "", "");
        check(value(open(callbackLoader, 31, "libregistry_ndk.so", null)) == 70,
                "constructor callback reenters the production registry");
        if (race) {
            race = false;
            resumeCreation();
            awaitCreationAttempt();
            check(value(open(callbackLoader, 31, "libregistry_ndk.so", null)) == 70,
                    "constructor reentry completes while another thread waits for namespace configuration");
        }
    }

    public static void main(String[] args) throws Exception {
        String root = args[0], alpha = root + "/alpha", beta = root + "/beta";
        setApplicationTargetSdk(23);
        check(applicationTargetSdk() == 23,
                "production application-target facade updates the authoritative native SDK");
        ClassLoader one = loader(null), two = loader(null);
        create(one, 31, false, alpha, "");
        create(two, 31, false, beta, "");
        check(applicationTargetSdk() == 23,
                "creating SDK31 classloader namespaces leaves the application target unchanged");
        setApplicationTargetSdk(0);
        check(applicationTargetSdk() == 36,
                "production application-target facade preserves zero-to-platform SDK semantics");
        long a = open(one, 31, "libsame.so", alpha);
        long b = open(two, 31, "libsame.so", beta);
        check(a != b && value(a) == 101 && value(b) == 201,
                "colliding SONAMEs retain each classloader's file and storage");
        check(open(one, 31, "libsame.so", null) == a,
                "distinct JNI argument references match the same Java object");
        check(open(one, 31, alpha + "/alias.so", null) == a && value(a) == 101,
                "same-file alias reuses mapping and constructor within the namespace");
        ClassLoader separate = loader(null);
        create(separate, 31, false, alpha, "");
        check(open(separate, 31, "libsame.so", null) != a,
                "independent namespaces do not inherit a private mapping");
        ClassLoader middle = loader(one), child = loader(middle);
        create(child, 31, true, alpha, "");
        check(open(child, 31, "libsame.so", null) == a,
                "shared namespace uses nearest registered ancestor through an unregistered parent");
        check(create(one, 31, false, alpha, alpha, "") != null,
                "duplicate explicit namespace creation reports an error");

        check(value(open(one, 31, "libregistry_ndk.so", null)) == 70,
                "NDK access needs no uses-native-library tag");
        absent(one, 31, "libregistry_device.so");
        absent(one, 31, "libregistry_private.so");
        ClassLoader declared = loader(null), all = loader(null), old = loader(null), unknown = loader(null);
        create(declared, 31, false, "", "libregistry_device.so");
        create(all, 31, false, "", "ALL");
        create(old, 30, false, "", "");
        for (ClassLoader loader : List.of(declared, all, old, unknown)) {
            check(value(open(loader, loader == old ? 30 : 31, "libregistry_device.so", null)) == 71,
                    "declared / ALL / SDK30 / unknown custom loader receives public device provider");
            absent(loader, 31, "libregistry_private.so");
        }

        ClassLoader files = loader(null);
        create(files, 31, false, "", "libpresent.vendor.so:libpresent.fixture.so");
        check(value(open(files, 31, "libpresent.vendor.so", null)) == 301,
                "declared vendor catalog file loads through a real linked ELF namespace");
        check(value(open(files, 31, "libpresent.fixture.so", null)) == 401,
                "declared system extension file loads through the same catalog policy");
        absent(one, 31, "libpresent.vendor.so");
        absent(files, 31, "libmissing.vendor.so");

        List<List<String>> catalog = new ArrayList<>();
        for (String[] row : builtins()) {
            check(row.length == 3, "builtin catalog uses copied three-column rows");
            catalog.add(Arrays.asList(row));
        }
        check(catalog.contains(List.of("libregistry_ndk.so", "ndk", "x86_64"))
                && catalog.contains(List.of("libregistry_device.so", "device", "x86_64"))
                && catalog.contains(List.of("libregistry_private.so", "private", "x86_64")),
                "builtin catalog preserves all three provider classifications");
        List<List<String>> fileRows = new ArrayList<>();
        for (String[] row : publicFiles()) fileRows.add(Arrays.asList(row));
        check(fileRows.contains(List.of("libpresent.vendor.so", "device", "x86_64"))
                && fileRows.contains(List.of("libpresent.fixture.so", "device", "x86_64"))
                && fileRows.stream().noneMatch(row -> row.get(0).contains("missing") || row.get(0).contains("only32")),
                "installed-file catalog requires real files and matching LP64 metadata");
        Files.delete(Path.of(root, "device/vendor/lib64/libpresent.vendor.so"));
        check(Arrays.stream(publicFiles()).noneMatch(row -> row[0].equals("libpresent.vendor.so")),
                "file catalog immediately stops advertising a removed provider");

        long before = loadedCount();
        long publicLinksBefore = publicLinkCount();
        check(publicLinksBefore > 0, "real public file-domain links exist before loader churn");
        long rssBefore = processRssKiB();
        List<WeakReference<ClassLoader>> weak = new ArrayList<>();
        for (int i = 0; i < 300; ++i) {
            ClassLoader transientLoader = loader(null);
            String error = create(transientLoader, 31, false, "", "", "");
            if (error != null) throw new AssertionError(error);
            weak.add(new WeakReference<>(transientLoader));
        }
        for (int i = 0; i < 30 && weak.stream().anyMatch(ref -> ref.get() != null); ++i) {
            System.gc();
            Thread.sleep(20);
        }
        check(weak.stream().allMatch(ref -> ref.get() == null),
                "native registry's real jweak identities do not keep 300 classloaders alive");
        create(loader(null), 31, false, "", "");
        check(loadedCount() == before,
                "creating and collecting empty classloaders adds no ELF mappings");
        check(publicLinkCount() == publicLinksBefore,
                "300 transient classloaders do not grow retained public file-domain links");
        long rssAfter = processRssKiB();
        check(rssBefore > 0 && rssAfter > 0, "host process RSS measurements are available");
        System.out.println("RESOURCE namespace_only_loaders=300 weak_refs_collected=300 elf_mappings_before="
                + before + " elf_mappings_after=" + loadedCount() + " process_rss_before_kib=" + rssBefore
                + " process_rss_after_kib=" + rssAfter + " retained_public_links_before=" + publicLinksBefore
                + " retained_public_links_after=" + publicLinkCount() + " (JVM and process allocations included)");
        ClassLoader racing = loader(null);
        create(racing, 31, false, root + "/race", "");
        creator = new Thread(() -> creationError = raceCreate(loader(null)), "namespace-creator");
        creator.start();
        awaitCreationSnapshot();
        race = true;
        check(value(open(racing, 31, "librace.so", null)) == 501,
                "real constructor finishes under concurrent classloader registration");
        creator.join(5000);
        check(!creator.isAlive() && creationError == null,
                "ordered ELF/registry locks publish the concurrent namespace without deadlock");
        System.out.println("NativeNamespaceRegistryTest all passed: " + checks + " checks; callbacks=" + callbacks);
        System.out.println("BOUNDARY: real HotSpot JNI; production registry/catalog/ELF; Linux Win32 primitives; namespace identities retained; no ART/Windows/unload assertion");
    }
}
