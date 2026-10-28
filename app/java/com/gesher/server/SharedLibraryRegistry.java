/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.internal.modules.utils.build.UnboundedSdkLevel;
import android.util.Xml;

import com.android.internal.pm.parsing.pkg.ParsedPackage;
import com.android.internal.util.XmlUtils;

import org.xmlpull.v1.XmlPullParser;

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

    public static SharedLibraryGraph resolve(ParsedPackage app) throws IOException {
        // Preserve the ordinary app's cheap startup path: no XML, hashing or SDK work.
        if (app.getUsesLibraries().isEmpty() && app.getUsesOptionalLibraries().isEmpty()) {
            return SharedLibraryGraph.forSystem(Collections.emptyList(), app.getClassLoaderName());
        }
        String root = System.getenv("ANDROID_ROOT");
        return read(new File(root == null || root.isEmpty() ? "/system" : root))
                .resolve(app.getUsesLibraries(), app.getUsesOptionalLibraries(), app.getClassLoaderName());
    }

    public static SharedLibraryRegistry read(File systemRoot) throws IOException {
        SharedLibraryRegistry registry = new SharedLibraryRegistry();
        for (String directory : new String[] {"etc/permissions", "etc/sysconfig"}) {
            File[] files = new File(systemRoot, directory).listFiles(
                    file -> file.isFile() && file.getName().endsWith(".xml"));
            if (files == null) continue;
            Arrays.sort(files, (first, second) -> first.getName().compareTo(second.getName()));
            for (File file : files) registry.readXml(file);
        }
        return registry;
    }

    private void readXml(File config) throws IOException {
        try (FileInputStream input = new FileInputStream(config)) {
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
                    File jar = new File(path);
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
