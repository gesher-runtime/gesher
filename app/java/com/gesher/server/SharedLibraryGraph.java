/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.pm.ApplicationInfo;
import android.content.pm.SharedLibraryInfo;
import android.content.pm.VersionedPackage;
import android.content.res.Resources;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/** One immutable Java system-library graph for LoadedApk and dex2oat. */
public final class SharedLibraryGraph {
    public static final String MANIFEST = "shared-libraries.txt";
    public static final SharedLibraryGraph EMPTY = new SharedLibraryGraph(
            Collections.emptyList(), Collections.emptySet());

    public static final class Node {
        public final String name;
        public final String path;
        public final List<Node> dependencies;
        private final File file;
        private final String contents;

        public Node(String name, File file, List<Node> dependencies) throws IOException {
            if (name == null || name.isEmpty()) throw new IllegalArgumentException("empty library name");
            this.name = name;
            this.file = file.getCanonicalFile();
            if (!this.file.isFile() || !this.file.canRead()) {
                throw new IOException("unreadable shared library " + name + ": " + this.file);
            }
            String location = this.file.getPath().replace('\\', '/');
            if (location.length() > 2 && location.charAt(1) == ':') {
                location = "/" + Character.toLowerCase(location.charAt(0)) + location.substring(2);
            }
            // ART's context grammar has no pathname escaping for these delimiters.
            for (char ch : "[]{}#;~:\n\r\t\0".toCharArray()) {
                if (location.indexOf(ch) >= 0) throw new IOException("unrepresentable ART library path: " + location);
            }
            path = location;
            contents = hash(this.file);
            this.dependencies = Collections.unmodifiableList(new ArrayList<>(
                    dependencies != null ? dependencies : Collections.emptyList()));
        }
    }

    private final List<Node> roots;
    private final Set<String> after;
    private final Map<String, Node> nodes = new LinkedHashMap<>();
    private final String context;
    private final String identity;

    public static SharedLibraryGraph forSystem(List<Node> roots) {
        return forSystem(roots, null);
    }

    public static SharedLibraryGraph forSystem(List<Node> roots, String appClassLoader) {
        String descriptor;
        if (appClassLoader == null || appClassLoader.equals("dalvik.system.PathClassLoader")) descriptor = "PCL";
        else if (appClassLoader.equals("dalvik.system.DelegateLastClassLoader")) descriptor = "DLC";
        else throw new IllegalArgumentException("unsupported app class loader context: " + appClassLoader);
        if (roots.isEmpty()) return descriptor.equals("PCL") ? EMPTY
                : new SharedLibraryGraph(roots, Collections.emptySet(), descriptor);
        Set<String> after = new HashSet<>();
        Collections.addAll(after, Resources.getSystem().getStringArray(
                com.android.internal.R.array.config_sharedLibrariesLoadedAfterApp));
        return new SharedLibraryGraph(roots, after, descriptor);
    }

    public SharedLibraryGraph(List<Node> roots, Set<String> after) {
        this(roots, after, "PCL");
    }

    private SharedLibraryGraph(List<Node> roots, Set<String> after, String descriptor) {
        this.roots = Collections.unmodifiableList(new ArrayList<>(roots));
        this.after = Collections.unmodifiableSet(new HashSet<>(after));
        Set<Node> visiting = Collections.newSetFromMap(new IdentityHashMap<>());
        for (Node root : roots) collect(root, visiting);
        context = descriptor + "[]" + libraries(roots);
        // Ordinary applications must retain the cheap PCL[] path and its startup behavior.
        if (roots.isEmpty() && descriptor.equals("PCL")) {
            identity = "";
        } else {
            MessageDigest digest = digest();
            field(digest, context);
            for (Node node : nodes.values()) {
                field(digest, node.name);
                field(digest, node.path);
                field(digest, node.contents);
            }
            identity = hex(digest.digest());
        }
    }

    private void collect(Node node, Set<Node> visiting) {
        if (!visiting.add(node)) throw new IllegalArgumentException("cyclic shared library " + node.name);
        Node old = nodes.get(node.name);
        if (old != null) {
            visiting.remove(node);
            if (old != node) throw new IllegalArgumentException("conflicting shared library " + node.name);
            return;
        }
        nodes.put(node.name, node);
        for (Node dependency : node.dependencies) collect(dependency, visiting);
        visiting.remove(node);
    }

    private String libraries(List<Node> libraries) {
        List<String> before = new ArrayList<>(), following = new ArrayList<>();
        for (Node node : libraries) {
            String value = "PCL[" + node.path + "]" + libraries(node.dependencies);
            if (after.contains(node.name)) following.add("~" + value); else before.add(value);
        }
        before.addAll(following);
        return before.isEmpty() ? "" : "{" + String.join("#", before) + "}";
    }

    public String classLoaderContext() { return context; }
    public String identity() { return identity; }
    public boolean isEmpty() { return roots.isEmpty(); }

    public boolean contentsUnchanged() throws IOException {
        for (Node node : nodes.values()) if (!node.contents.equals(hash(node.file))) return false;
        return true;
    }

    public void apply(ApplicationInfo app) {
        if (roots.isEmpty()) {
            app.sharedLibraryInfos = null;
            app.sharedLibraryFiles = null;
            return;
        }
        Map<Node, SharedLibraryInfo> infos = new IdentityHashMap<>();
        List<SharedLibraryInfo> rootsInfo = new ArrayList<>();
        for (Node node : roots) rootsInfo.add(info(node, infos));
        app.sharedLibraryInfos = rootsInfo;
        Set<String> files = new LinkedHashSet<>();
        for (Node node : nodes.values()) files.add(node.path);
        app.sharedLibraryFiles = files.toArray(new String[0]);
    }

    private SharedLibraryInfo info(Node node, Map<Node, SharedLibraryInfo> infos) {
        SharedLibraryInfo known = infos.get(node);
        if (known != null) return known;
        List<SharedLibraryInfo> dependencies = new ArrayList<>();
        for (Node dependency : node.dependencies) dependencies.add(info(dependency, infos));
        SharedLibraryInfo result = new SharedLibraryInfo(node.path, null, null, node.name,
                SharedLibraryInfo.VERSION_UNDEFINED, SharedLibraryInfo.TYPE_BUILTIN,
                new VersionedPackage("android", 0), null, dependencies, false);
        infos.put(node, result);
        return result;
    }

    public void writeManifest(File directory) throws IOException {
        StringBuilder text = new StringBuilder("gesher-shared-library-context-v1\n")
                .append(identity).append('\n').append(context).append('\n');
        for (Node node : nodes.values()) {
            text.append(node.name).append('\t').append(node.path).append('\t')
                    .append(node.contents).append('\n');
        }
        Files.write(new File(directory, MANIFEST).toPath(), text.toString().getBytes(StandardCharsets.UTF_8));
    }

    private static MessageDigest digest() {
        try { return MessageDigest.getInstance("SHA-256"); }
        catch (NoSuchAlgorithmException impossible) { throw new AssertionError(impossible); }
    }

    private static String hash(File file) throws IOException {
        MessageDigest digest = digest();
        try (FileInputStream stream = new FileInputStream(file)) {
            byte[] buffer = new byte[64 * 1024];
            for (int count; (count = stream.read(buffer)) != -1;) digest.update(buffer, 0, count);
        }
        return hex(digest.digest());
    }

    private static void field(MessageDigest digest, String value) {
        byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
        for (int shift = 24; shift >= 0; shift -= 8) digest.update((byte)(bytes.length >>> shift));
        digest.update(bytes);
    }

    private static String hex(byte[] bytes) {
        StringBuilder text = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) {
            text.append(Character.forDigit((value >>> 4) & 15, 16));
            text.append(Character.forDigit(value & 15, 16));
        }
        return text.toString();
    }
}
