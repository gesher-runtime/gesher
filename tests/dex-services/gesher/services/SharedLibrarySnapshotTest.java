/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.content.pm.ApplicationInfo;
import android.content.res.AssetManager;
import android.os.StrictMode;
import android.os.strictmode.Violation;
import android.system.Os;

import com.gesher.server.SharedLibraryGraph;
import com.gesher.server.SharedLibraryRegistry;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * The system root's shared-library declarations are read once and kept while their files
 * are unchanged (SharedLibraryRegistry's snapshot), as SystemConfig reads them once at
 * boot: a bindService that resolves a service's ApplicationInfo read and parsed every XML
 * file under etc/permissions and etc/sysconfig and hashed every jar again, and the app's
 * StrictMode reported the system server's unbuffered reads (UnbufferedIoViolation). A
 * changed, added or removed declaration or jar is still seen at the next resolution.
 * args[0]: a scratch directory. Prints "SharedLibrarySnapshotTest all passed".
 */
public final class SharedLibrarySnapshotTest {
    private static final String LIBRARY = "gesher.snapshot.entry";
    private static final String OTHER = "gesher.snapshot.other";

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void declare(File root, String xml) throws IOException {
        File file = new File(root, "etc/permissions/snapshot.xml");
        file.getParentFile().mkdirs();
        Files.write(file.toPath(), ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<permissions>\n"
                + xml + "</permissions>\n").getBytes(StandardCharsets.UTF_8));
    }

    private static String library(String name) {
        return "    <library name=\"" + name + "\" file=\"/system/framework/" + name + ".jar\" />\n";
    }

    private static void write(File file, int size) throws IOException {
        file.getParentFile().mkdirs();
        byte[] bytes = new byte[size];
        for (int i = 0; i < size; i++) bytes[i] = (byte) (i * 31 + size);
        Files.write(file.toPath(), bytes);
    }

    private static SharedLibraryGraph resolve(String library) throws IOException {
        return SharedLibraryRegistry.resolveSystem(36, Collections.singletonList(library),
                Collections.emptyList(), null);
    }

    public static void main(String[] args) throws Exception {
        // android.util.Xml reads a system property, and the graph the system resources.
        System.loadLibrary("android_runtime");
        String systemRoot = System.getenv("ANDROID_ROOT");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                systemRoot + "/framework/framework-res.apk");
        File root = new File(args[0], "snapshot-system").getCanonicalFile();
        File jar = new File(root, "framework/" + LIBRARY + ".jar");
        write(jar, 4096);
        declare(root, library(LIBRARY));
        // The registry follows $ANDROID_ROOT, as in the system server.
        Os.setenv("ANDROID_ROOT", root.getPath(), true);
        try {
            int reads = SharedLibraryRegistry.systemReads();
            SharedLibraryGraph first = resolve(LIBRARY);
            require(SharedLibraryRegistry.systemReads() == reads + 1,
                    "the first resolution did not read the declarations once");

            // Unchanged files: the same graph, nothing read or hashed again, and nothing
            // the calling thread's StrictMode policy reports.
            List<Violation> violations = new ArrayList<>();
            StrictMode.ThreadPolicy before = StrictMode.getThreadPolicy();
            StrictMode.setThreadPolicy(new StrictMode.ThreadPolicy.Builder()
                    .detectDiskReads().detectUnbufferedIo()
                    .penaltyListener(Runnable::run, violations::add).build());
            SharedLibraryGraph again;
            try {
                again = resolve(LIBRARY);
                SharedLibraryRegistry.readSystem();
            } finally {
                StrictMode.setThreadPolicy(before);
            }
            require(again == first, "an unchanged configuration resolved a new graph");
            require(SharedLibraryRegistry.systemReads() == reads + 1,
                    "an unchanged configuration was read again");
            require(violations.isEmpty(), "the cached resolution reported " + violations);
            System.out.println("SharedLibrarySnapshotTest: unchanged declarations are read once "
                    + "and their graph reused, with no StrictMode violation for the caller");

            // A jar replaced in place: read again, a new graph with the new contents.
            write(jar, 8192);
            SharedLibraryGraph replaced = resolve(LIBRARY);
            require(replaced != first && !replaced.identity().equals(first.identity())
                            && !first.contentsUnchanged() && replaced.contentsUnchanged(),
                    "a replaced jar kept the old graph");
            require(SharedLibraryRegistry.systemReads() == reads + 2,
                    "a replaced jar was not seen at the next resolution");

            // A declaration added to the XML, under its StrictMode policy: the new
            // library resolves, read with buffered input (no UnbufferedIoViolation).
            write(new File(root, "framework/" + OTHER + ".jar"), 1024);
            declare(root, library(LIBRARY) + library(OTHER));
            StrictMode.setThreadPolicy(new StrictMode.ThreadPolicy.Builder()
                    .detectUnbufferedIo().penaltyListener(Runnable::run, violations::add).build());
            SharedLibraryGraph other;
            try {
                other = resolve(OTHER);
            } finally {
                StrictMode.setThreadPolicy(before);
            }
            ApplicationInfo app = new ApplicationInfo();
            other.apply(app);
            require(app.sharedLibraryFiles != null && app.sharedLibraryFiles.length == 1
                            && app.sharedLibraryFiles[0].endsWith("/" + OTHER + ".jar"),
                    "an added declaration did not resolve to its jar");
            require(SharedLibraryRegistry.systemReads() == reads + 3,
                    "an added declaration was not read");
            require(violations.isEmpty(), "reading the declarations reported " + violations);

            // A declared jar removed, then the declaration: each is seen.
            require(new File(root, "framework/" + OTHER + ".jar").delete(), "delete the jar");
            try {
                resolve(OTHER);
                throw new AssertionError("a removed jar still resolved");
            } catch (IOException expected) {
                require(expected.getMessage().contains("INSTALL_FAILED_MISSING_SHARED_LIBRARY"),
                        "a removed jar failed with " + expected.getMessage());
            }
            declare(root, "");
            try {
                resolve(LIBRARY);
                throw new AssertionError("a removed declaration still resolved");
            } catch (IOException expected) {
                require(expected.getMessage().contains("INSTALL_FAILED_MISSING_SHARED_LIBRARY"),
                        "a removed declaration failed with " + expected.getMessage());
            }
            System.out.println("SharedLibrarySnapshotTest: replaced, added and removed jars and "
                    + "declarations are seen at the next resolution");
        } finally {
            Os.setenv("ANDROID_ROOT", systemRoot, true);
        }
        System.out.println("SharedLibrarySnapshotTest all passed");
    }
}
