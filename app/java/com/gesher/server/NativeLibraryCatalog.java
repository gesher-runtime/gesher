/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.PackageParser.PackageParserException;
import android.content.pm.SharedLibraryInfo;
import android.content.pm.VersionedPackage;

import com.android.internal.pm.parsing.pkg.ParsedPackage;
import com.gesher.app.NativeLibs;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * The device's native declarations backed by available providers and real ELF
 * files. The native namespace adapter reads the same catalog. NDK libraries
 * remain accessible without a manifest declaration; they and private runtime
 * libraries are not device-maker entries in PackageManager's shared libraries.
 */
public final class NativeLibraryCatalog {
    private final Map<String, Set<String>> deviceLibraries = new LinkedHashMap<>();

    private NativeLibraryCatalog() {}

    /** A fresh snapshot, including provider/backend and installed-file changes. */
    public static NativeLibraryCatalog readSystem() throws IOException {
        NativeLibraryCatalog catalog = new NativeLibraryCatalog();
        try {
            catalog.add(nativeBuiltinCatalog());
            catalog.add(nativePublicFileCatalog());
        } catch (UnsatisfiedLinkError unavailable) {
            // An older runtime cannot establish native availability. In
            // particular, required declarations must not become successful.
            throw new IOException("The runtime cannot enumerate native libraries", unavailable);
        }
        return catalog;
    }

    private void add(String[][] rows) throws IOException {
        if (rows == null) throw new IOException("No native library catalog returned");
        for (String[] row : rows) {
            if (row == null || row.length != 3 || row[0] == null || row[0].isEmpty()
                    || row[0].indexOf('/') >= 0 || row[0].indexOf('\\') >= 0
                    || row[0].indexOf(':') >= 0
                    || !("ndk".equals(row[1]) || "device".equals(row[1]) || "private".equals(row[1]))
                    || !Arrays.asList(NativeLibs.SUPPORTED).contains(row[2])) {
                throw new IOException("Invalid native library catalog row");
            }
            if ("device".equals(row[1])) {
                deviceLibraries.computeIfAbsent(row[0], key -> new LinkedHashSet<>()).add(row[2]);
            }
        }
    }

    public String[] names() { return deviceLibraries.keySet().toArray(new String[0]); }

    /** Resolve precisely the native ABI selected for the complete APK set. */
    public static List<SharedLibraryInfo> resolve(ParsedPackage app, String abi)
            throws IOException, PackageParserException {
        if (!CompatChanges.isEnabled(CompatChanges.ENFORCE_NATIVE_SHARED_LIBRARY_DEPENDENCIES,
                app.getTargetSdkVersion())) return Collections.emptyList();
        List<String> required = app.getUsesNativeLibraries();
        List<String> optional = app.getUsesOptionalNativeLibraries();
        if (required.isEmpty() && optional.isEmpty()) return Collections.emptyList();
        NativeLibraryCatalog catalog = readSystem();
        String selectedAbi = abi != null ? abi : NativeLibs.ABI;
        List<SharedLibraryInfo> result = new ArrayList<>();
        Set<String> seen = new LinkedHashSet<>();
        for (String name : required) {
            if (!catalog.available(name, selectedAbi)) {
                throw new PackageParserException(PackageManager.INSTALL_FAILED_MISSING_SHARED_LIBRARY,
                        "INSTALL_FAILED_MISSING_SHARED_LIBRARY: " + app.getPackageName()
                                + " requires native library " + name + " for " + selectedAbi);
            }
            if (seen.add(name)) result.add(info(name));
        }
        for (String name : optional) {
            if (catalog.available(name, selectedAbi) && seen.add(name)) result.add(info(name));
        }
        return result;
    }

    private boolean available(String name, String abi) {
        Set<String> supported = deviceLibraries.get(name);
        return supported != null && supported.contains(abi);
    }

    private static SharedLibraryInfo info(String name) {
        // AOSP's native SharedLibraryEntry uses the SONAME as its path. This
        // object conveys a native allowlist to LoadedApk, never a dex path.
        return new SharedLibraryInfo(name, null, null, name, SharedLibraryInfo.VERSION_UNDEFINED,
                SharedLibraryInfo.TYPE_BUILTIN, new VersionedPackage("android", 0),
                null, null, true);
    }

    /** Invoke after SharedLibraryGraph.apply; leave its dex files and identity intact. */
    public static void apply(ApplicationInfo app, List<SharedLibraryInfo> nativeLibraries) {
        if (nativeLibraries.isEmpty()) return;
        List<SharedLibraryInfo> merged = new ArrayList<>();
        if (app.sharedLibraryInfos != null) merged.addAll(app.sharedLibraryInfos);
        merged.addAll(nativeLibraries);
        app.sharedLibraryInfos = merged;
    }

    private static native String[][] nativeBuiltinCatalog();
    private static native String[][] nativePublicFileCatalog() throws IOException;
}
