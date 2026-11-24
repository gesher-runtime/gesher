/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.internal.modules.utils.build.UnboundedSdkLevel;
import android.os.Build;
import android.os.StrictMode;
import android.util.Xml;

import com.android.internal.pm.parsing.pkg.ParsedPackage;
import com.android.internal.util.XmlUtils;

import org.xmlpull.v1.XmlPullParser;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/** Java system libraries from the device's permissions/sysconfig XML files. */
public final class SharedLibraryRegistry {
    /** Apache HTTP, which apps targeting API 27 or lower get without naming it. */
    public static final String ORG_APACHE_HTTP_LEGACY = "org.apache.http.legacy";
    /** JUnit's framework and android.test's base classes, implicit up to API 29. */
    public static final String ANDROID_TEST_BASE = "android.test.base";
    /** android.test's mocks, split out of android.test.runner. */
    public static final String ANDROID_TEST_MOCK = "android.test.mock";
    /** android.test's instrumentation runner. */
    public static final String ANDROID_TEST_RUNNER = "android.test.runner";
    private static final String SYSTEM = "/system/";
    private static final String[] CONFIG_DIRECTORIES = {"etc/permissions", "etc/sysconfig"};

    private static final class Entry {
        final String name;
        final File file;
        final List<String> dependencies;
        final boolean onBootClassPath;

        Entry(String name, File file, String dependencies, boolean onBootClassPath) {
            this.name = name;
            this.file = file;
            this.dependencies = dependencies == null || dependencies.isEmpty()
                    ? Collections.emptyList() : Arrays.asList(dependencies.split(":"));
            this.onBootClassPath = onBootClassPath;
        }
    }

    private final Map<String, Entry> entries = new LinkedHashMap<>();
    /** Every library file a declaration in range names, present or not. */
    private final Set<File> declaredFiles = new LinkedHashSet<>();

    /** An application's libraries after PackageBackwardCompatibility's implicit ones. */
    public static final class Uses {
        public final List<String> required;
        public final List<String> optional;

        Uses(List<String> required, List<String> optional) {
            this.required = Collections.unmodifiableList(required);
            this.optional = Collections.unmodifiableList(optional);
        }
    }

    /**
     * The system root's declarations, read once and kept while its configuration is
     * unchanged, as SystemConfig reads them once at boot. Gesher's root can be updated
     * while its system server runs, so each use compares the files' metadata (not their
     * contents) and reads again after a change. Graphs resolved from it are kept with
     * it: resolving hashes every jar, which a binder call (bindService resolving a
     * service's ApplicationInfo) should not repeat.
     */
    private static final class Snapshot {
        final File root;
        final List<String> stamp;
        final SharedLibraryRegistry registry;
        final Map<List<Object>, SharedLibraryGraph> graphs = new HashMap<>();

        Snapshot(File root, List<String> stamp, SharedLibraryRegistry registry) {
            this.root = root;
            this.stamp = stamp;
            this.registry = registry;
        }
    }

    private static final Object sLock = new Object();
    private static Snapshot sSnapshot;
    private static int sReads;

    /** Whether the app names no library and gets none implicitly. */
    public static boolean usesNone(ParsedPackage app) {
        // AndroidTestBaseUpdater adds android.test.base up to API 29 (Q), and
        // OrgApacheHttpLegacyUpdater org.apache.http.legacy up to API 27.
        return app.getUsesLibraries().isEmpty() && app.getUsesOptionalLibraries().isEmpty()
                && app.getTargetSdkVersion() > Build.VERSION_CODES.Q;
    }

    public static SharedLibraryGraph resolve(ParsedPackage app) throws IOException {
        // Preserve the ordinary app's cheap startup path: no XML, hashing or SDK work.
        if (usesNone(app)) {
            return SharedLibraryGraph.forSystem(Collections.emptyList(), app.getClassLoaderName());
        }
        return resolveSystem(app.getTargetSdkVersion(), app.getUsesLibraries(),
                app.getUsesOptionalLibraries(), app.getClassLoaderName());
    }

    /**
     * An application's graph from the system root's declarations: the libraries it
     * names, with the implicit ones, resolved once per configuration snapshot.
     */
    public static SharedLibraryGraph resolveSystem(int targetSdk, List<String> required,
            List<String> optional, String appClassLoader) throws IOException {
        // The system server's own configuration work, which Android's package manager
        // does at boot, not disk access of the app whose binder call this may serve.
        StrictMode.ThreadPolicy policy = StrictMode.allowThreadDiskReads();
        try {
            Snapshot snapshot = systemSnapshot();
            Uses uses = snapshot.registry.withImplicit(targetSdk, required, optional);
            List<Object> key = Arrays.asList(uses.required, uses.optional, appClassLoader);
            synchronized (snapshot.graphs) {
                SharedLibraryGraph graph = snapshot.graphs.get(key);
                if (graph == null) {
                    graph = snapshot.registry.resolve(uses.required, uses.optional, appClassLoader);
                    snapshot.graphs.put(key, graph);
                }
                return graph;
            }
        } finally {
            StrictMode.setThreadPolicy(policy);
        }
    }

    /** The same installed declarations used when resolving an application's libraries. */
    public static SharedLibraryRegistry readSystem() throws IOException {
        StrictMode.ThreadPolicy policy = StrictMode.allowThreadDiskReads();
        try {
            return systemSnapshot().registry;
        } finally {
            StrictMode.setThreadPolicy(policy);
        }
    }

    /** How many times this process has read the system root's declarations. */
    public static int systemReads() {
        synchronized (sLock) {
            return sReads;
        }
    }

    private static Snapshot systemSnapshot() throws IOException {
        String path = System.getenv("ANDROID_ROOT");
        File root = new File(path == null || path.isEmpty() ? "/system" : path);
        synchronized (sLock) {
            Snapshot snapshot = sSnapshot;
            if (snapshot != null && snapshot.root.equals(root)
                    && snapshot.stamp.equals(stamp(root, snapshot.registry.declaredFiles))) {
                return snapshot;
            }
            // The configuration files' part of the stamp is taken before they are read:
            // a change while reading leaves a stamp that no longer matches, so the next
            // use reads again. The jars' part follows from the declarations just read.
            List<String> stamp = stamp(root, Collections.emptySet());
            SharedLibraryRegistry registry = read(root);
            stamp.addAll(stamp(null, registry.declaredFiles));
            sReads++;
            sSnapshot = new Snapshot(root, Collections.unmodifiableList(stamp), registry);
            return sSnapshot;
        }
    }

    /**
     * Metadata that changes with a declaration or a declared jar: each configuration
     * file's name, size and modification time (when a root is given), and each declared
     * library file's presence, size and modification time.
     */
    private static List<String> stamp(File root, Set<File> libraries) {
        List<String> stamp = new ArrayList<>();
        if (root != null) {
            for (String directory : CONFIG_DIRECTORIES) {
                File[] files = configFiles(new File(root, directory));
                stamp.add(directory + (files == null ? " absent" : " " + files.length));
                if (files == null) continue;
                for (File file : files) {
                    stamp.add(file.getName() + '\t' + file.length() + '\t' + file.lastModified());
                }
            }
        }
        for (File library : libraries) {
            stamp.add(library.getPath() + '\t' + library.isFile() + '\t' + library.length()
                    + '\t' + library.lastModified());
        }
        return stamp;
    }

    private static File[] configFiles(File directory) {
        File[] files = directory.listFiles(file -> file.isFile() && file.getName().endsWith(".xml"));
        if (files != null) {
            Arrays.sort(files, (first, second) -> first.getName().compareTo(second.getName()));
        }
        return files;
    }

    /** A fresh snapshot; absent files and SDK-excluded declarations were filtered by read. */
    public String[] names() {
        return entries.keySet().toArray(new String[0]);
    }

    /**
     * The libraries with the ones Android adds, in PackageBackwardCompatibility's order:
     * OrgApacheHttpLegacyUpdater (org.apache.http.legacy first for apps targeting API 27
     * or lower), AndroidTestRunnerSplitUpdater (android.test.mock with
     * android.test.runner) and AndroidTestBaseUpdater (android.test.base for apps
     * targeting API 29 or lower, and with android.test.runner above). Only libraries the
     * device declares are added, so a system root without one still installs those
     * apps. Gesher has no per-package compat overrides, so REMOVE_ANDROID_TEST_BASE
     * takes its target SDK default, as AndroidTestBaseUpdater's fallback does.
     */
    public Uses withImplicit(int targetSdk, List<String> required, List<String> optional) {
        List<String> uses = new ArrayList<>(required);
        List<String> usesOptional = new ArrayList<>(optional);
        if (targetSdk <= Build.VERSION_CODES.O_MR1) {
            prefixRequired(uses, usesOptional, ORG_APACHE_HTTP_LEGACY);
        }
        prefixImplicitDependency(uses, usesOptional, ANDROID_TEST_RUNNER, ANDROID_TEST_MOCK);
        if (targetSdk <= Build.VERSION_CODES.Q) {
            prefixRequired(uses, usesOptional, ANDROID_TEST_BASE);
        } else {
            prefixImplicitDependency(uses, usesOptional, ANDROID_TEST_RUNNER, ANDROID_TEST_BASE);
        }
        return new Uses(uses, usesOptional);
    }

    // PackageSharedLibraryUpdater.prefixRequiredLibrary, for a declared library.
    private void prefixRequired(List<String> required, List<String> optional, String library) {
        if (entries.containsKey(library) && !required.contains(library)
                && !optional.contains(library)) {
            required.add(0, library);
        }
    }

    // PackageSharedLibraryUpdater.prefixImplicitDependency, for a declared library.
    private void prefixImplicitDependency(List<String> required, List<String> optional,
            String existing, String implicit) {
        if (!entries.containsKey(implicit) || required.contains(implicit)
                || optional.contains(implicit)) {
            return;
        }
        if (required.contains(existing)) {
            required.add(0, implicit);
        } else if (optional.contains(existing)) {
            optional.add(0, implicit);
        }
    }

    public static SharedLibraryRegistry read(File systemRoot) throws IOException {
        SharedLibraryRegistry registry = new SharedLibraryRegistry();
        for (String directory : CONFIG_DIRECTORIES) {
            File[] files = configFiles(new File(systemRoot, directory));
            if (files == null) continue;
            for (File file : files) registry.readXml(systemRoot, file);
        }
        return registry;
    }

    private void readXml(File systemRoot, File config) throws IOException {
        // Buffered: the parser reads its input a few bytes at a time.
        try (BufferedInputStream input = new BufferedInputStream(new FileInputStream(config))) {
            XmlPullParser parser = Xml.newPullParser();
            parser.setInput(input, null);
            int event;
            do { event = parser.next(); }
            while (event != XmlPullParser.START_TAG && event != XmlPullParser.END_DOCUMENT);
            if (event != XmlPullParser.START_TAG || (!parser.getName().equals("permissions")
                    && !parser.getName().equals("config"))) {
                throw new IOException("expected permissions/config root in " + config);
            }
            int depth = parser.getDepth();
            while (XmlUtils.nextElementWithin(parser, depth)) {
                if (!parser.getName().equals("library")) {
                    XmlUtils.skipCurrentTag(parser);
                    continue;
                }
                String name = parser.getAttributeValue(null, "name");
                String path = parser.getAttributeValue(null, "file");
                String min = parser.getAttributeValue(null, "min-device-sdk");
                String max = parser.getAttributeValue(null, "max-device-sdk");
                if (name != null && !name.isEmpty() && path != null && !path.isEmpty()
                        && (min == null || atLeast(min)) && (max == null || atMost(max))) {
                    // Android's /system paths are under the device's system root.
                    File jar = path.startsWith(SYSTEM)
                            ? new File(systemRoot, path.substring(SYSTEM.length())) : new File(path);
                    declaredFiles.add(jar);
                    // SystemConfig ignores missing/out-of-range library declarations.
                    if (jar.isFile()) {
                        String since = parser.getAttributeValue(null, "on-bootclasspath-since");
                        String before = parser.getAttributeValue(null, "on-bootclasspath-before");
                        boolean boot = (since != null && atLeast(since))
                                || (before != null && !atLeast(before));
                        entries.put(name, new Entry(name, jar,
                                parser.getAttributeValue(null, "dependency"), boot));
                    }
                }
                XmlUtils.skipCurrentTag(parser);
            }
        } catch (org.xmlpull.v1.XmlPullParserException | IllegalArgumentException e) {
            throw new IOException("invalid shared-library configuration " + config, e);
        }
    }

    // These are exactly SystemConfig's wrappers around the pinned SDK helper.
    private static boolean atLeast(String version) {
        try { return UnboundedSdkLevel.isAtLeast(version); }
        catch (IllegalArgumentException oldCodename) { return false; }
    }

    private static boolean atMost(String version) {
        try { return UnboundedSdkLevel.isAtMost(version); }
        catch (IllegalArgumentException oldCodename) { return true; }
    }

    public SharedLibraryGraph resolve(List<String> required, List<String> optional,
            String appClassLoader) throws IOException {
        Map<String, SharedLibraryGraph.Node> resolved = new HashMap<>();
        List<SharedLibraryGraph.Node> roots = new ArrayList<>();
        Set<String> names = new LinkedHashSet<>(required);
        names.addAll(optional);
        for (String name : names) {
            if (!entries.containsKey(name)) {
                if (required.contains(name)) throw new IOException("INSTALL_FAILED_MISSING_SHARED_LIBRARY: " + name);
                continue;
            }
            SharedLibraryGraph.Node node = resolve(name, resolved, new HashSet<>());
            if (node != null) roots.add(node);
        }
        return SharedLibraryGraph.forSystem(roots, appClassLoader);
    }

    private SharedLibraryGraph.Node resolve(String name, Map<String, SharedLibraryGraph.Node> resolved,
            Set<String> visiting) throws IOException {
        if (resolved.containsKey(name)) return resolved.get(name);
        Entry entry = entries.get(name);
        if (entry == null) throw new IOException("missing shared-library dependency " + name);
        if (!visiting.add(name)) throw new IOException("cyclic shared-library dependency " + name);
        if (entry.onBootClassPath) {
            // A declaration must not manufacture a boot dependency absent from this runtime.
            if (!onActualBootClassPath(entry.file)) {
                throw new IOException("shared library declared on boot class path but absent: " + name);
            }
            resolved.put(name, null);
            visiting.remove(name);
            return null;
        }
        List<SharedLibraryGraph.Node> dependencies = new ArrayList<>();
        for (String dependency : entry.dependencies) {
            SharedLibraryGraph.Node node = resolve(dependency, resolved, visiting);
            if (node != null && !dependencies.contains(node)) dependencies.add(node);
        }
        SharedLibraryGraph.Node node = new SharedLibraryGraph.Node(entry.name, entry.file, dependencies);
        resolved.put(name, node);
        visiting.remove(name);
        return node;
    }

    private static boolean onActualBootClassPath(File file) throws IOException {
        File canonical = file.getCanonicalFile();
        String classPath = System.getProperty("java.boot.class.path", "");
        for (String path : classPath.split(File.pathSeparator)) {
            if (!path.isEmpty() && new File(path).getCanonicalFile().equals(canonical)) return true;
        }
        return false;
    }
}
