/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Enumeration;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipException;
import java.util.zip.ZipFile;

/** ZIP containers holding the APKs for a single app installation. */
public final class PackageBundles {
    private PackageBundles() {}

    /**
     * Whether {@code file} is a ZIP of APKs (an APK set, .xapk, .apkm, .apk+) rather than an
     * APK, by its content: an APK is a ZIP with {@code AndroidManifest.xml} at its root, a
     * bundle has none there and holds {@code .apk} entries. The file name is not consulted,
     * so a single APK saved as {@code .xapk} installs as the APK it is. A file that is not a
     * ZIP is not a bundle; the APK parser reports it.
     */
    public static boolean isBundle(File file) throws IOException {
        try (ZipFile zip = new ZipFile(file)) {
            if (zip.getEntry("AndroidManifest.xml") != null) return false;
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
                ZipEntry entry = e.nextElement();
                if (!entry.isDirectory() && entry.getName().endsWith(".apk")) return true;
            }
            return false;
        } catch (ZipException notZip) {
            return false;
        }
    }

    /**
     * The device an APK set is matched against, as bundletool's {@code DeviceSpec} describes
     * one: its SDK level, its ABIs in order of preference, its density (0: every density
     * split is kept) and its languages (null: every language split is kept).
     */
    public static final class Device {
        final int sdk;
        final List<String> abis;
        final int densityDpi;
        final Set<String> languages;

        public Device(int sdk, String[] abis, int densityDpi, String[] locales) {
            this.sdk = sdk;
            this.abis = Collections.unmodifiableList(Arrays.asList(abis.clone()));
            this.densityDpi = densityDpi;
            if (locales == null) {
                languages = null;
            } else {
                Set<String> set = new LinkedHashSet<>();
                for (String locale : locales) set.add(ApkSetToc.language(locale));
                languages = Collections.unmodifiableSet(set);
            }
        }

        /**
         * Gesher's device at SDK level {@code sdk}: {@link NativeLibs#SUPPORTED}, with every
         * density and language split. The density is chosen per launch (GESHER_DISPLAY) and
         * the languages follow Windows' settings and per-app locales at any time, while no
         * store delivers a missing split later, so these are all installed and resources
         * pick among them at run time, as for a universal APK.
         */
        public static Device gesher(int sdk) {
            return new Device(sdk, NativeLibs.SUPPORTED, 0, null);
        }

        @Override public String toString() {
            return "SDK " + sdk + ", ABIs " + String.join(", ", abis)
                    + (densityDpi != 0 ? ", " + densityDpi + " dpi" : "")
                    + (languages != null ? ", languages " + String.join(", ", languages) : "");
        }
    }

    /** Whether {@code file} can be read as a ZIP (an encrypted ZIP or APKM cannot). */
    public static boolean isZip(File file) throws IOException {
        try (ZipFile zip = new ZipFile(file)) {
            return true;
        } catch (ZipException notZip) {
            return false;
        }
    }

    /** The most a toc.pb or manifest.json is read into memory (they are kilobytes). */
    private static final int MAX_TOC = 16 * 1024 * 1024;

    /**
     * The archive paths of the APKs that make up the install from bundle {@code zip}: for a
     * bundletool APK set (it has {@code toc.pb}), those {@code device} gets, as
     * {@code bundletool install-apks} chooses them ({@link ApkSetToc}); for an XAPK whose
     * {@code manifest.json} lists {@code split_apks}, those files; otherwise every
     * {@code .apk} entry, in archive order.
     */
    public static List<String> select(ZipFile zip, Device device) throws IOException {
        ZipEntry toc = zip.getEntry("toc.pb");
        if (toc != null) {
            List<String> paths;
            try {
                paths = ApkSetToc.select(read(zip, toc), device);
            } catch (IllegalArgumentException invalid) {
                throw new IOException("Invalid toc.pb", invalid);
            }
            return held(zip, paths, "toc.pb");
        }
        Map<?, ?> manifest = xapkManifest(zip);
        if (manifest != null && manifest.get("split_apks") instanceof List) {
            List<String> paths = new ArrayList<>();
            for (Object split : (List<?>) manifest.get("split_apks")) {
                Object file = split instanceof Map ? ((Map<?, ?>) split).get("file") : null;
                if (!(file instanceof String)) throw new IOException("Invalid manifest.json: split_apks");
                paths.add((String) file);
            }
            return held(zip, paths, "manifest.json");
        }
        List<String> paths = new ArrayList<>();
        for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
            ZipEntry entry = e.nextElement();
            if (!entry.isDirectory() && entry.getName().endsWith(".apk")) paths.add(entry.getName());
        }
        return paths;
    }

    /**
     * The expansion (OBB) files in bundle {@code zip}: those an XAPK's {@code manifest.json}
     * lists under {@code expansions}, and any {@code .obb} entry. Android keeps them in shared
     * storage, {@code Android/obb/<package>/}, where Google Play or an XAPK installer puts
     * them; a package install does not.
     */
    public static List<String> expansions(ZipFile zip) throws IOException {
        Set<String> paths = new LinkedHashSet<>();
        Map<?, ?> manifest = xapkManifest(zip);
        if (manifest != null && manifest.get("expansions") instanceof List) {
            for (Object expansion : (List<?>) manifest.get("expansions")) {
                Object file = expansion instanceof Map ? ((Map<?, ?>) expansion).get("file") : null;
                if (file instanceof String) paths.add((String) file);
            }
        }
        for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
            ZipEntry entry = e.nextElement();
            if (!entry.isDirectory() && entry.getName().toLowerCase(Locale.ROOT).endsWith(".obb")) {
                paths.add(entry.getName());
            }
        }
        return new ArrayList<>(paths);
    }

    private static byte[] read(ZipFile zip, ZipEntry entry) throws IOException {
        if (entry.getSize() > MAX_TOC) throw new IOException(entry.getName() + " is too large");
        byte[] bytes;
        try (InputStream in = zip.getInputStream(entry)) {
            bytes = in.readNBytes(MAX_TOC + 1);
        }
        if (bytes.length > MAX_TOC) throw new IOException(entry.getName() + " is too large");
        return bytes;
    }

    /** {@code paths}, after checking that each is an APK the archive holds. */
    private static List<String> held(ZipFile zip, List<String> paths, String index) throws IOException {
        for (String path : paths) {
            ZipEntry entry = zip.getEntry(path);
            if (entry == null || entry.isDirectory() || !path.endsWith(".apk")) {
                throw new IOException(index + " names " + path + ", which the archive does not hold");
            }
        }
        return paths;
    }

    /** An XAPK's manifest.json (APKPure's format), or null when the archive has none. */
    private static Map<?, ?> xapkManifest(ZipFile zip) throws IOException {
        ZipEntry entry = zip.getEntry("manifest.json");
        if (entry == null || entry.isDirectory()) return null;
        Object value;
        try {
            value = new Json(new String(read(zip, entry), StandardCharsets.UTF_8)).document();
        } catch (IllegalArgumentException invalid) {
            throw new IOException("Invalid manifest.json", invalid);
        }
        return value instanceof Map ? (Map<?, ?>) value : null;
    }

    /** A JSON (RFC 8259) reader: objects as maps, arrays as lists, numbers as Double. */
    private static final class Json {
        private final String text;
        private int at;

        Json(String text) {
            this.text = text;
        }

        Object document() {
            Object value = value(0);
            space();
            if (at != text.length()) throw error();
            return value;
        }

        private Object value(int depth) {
            if (depth > 64) throw error();
            space();
            if (at >= text.length()) throw error();
            char c = text.charAt(at);
            if (c == '{') {
                at++;
                Map<String, Object> map = new LinkedHashMap<>();
                space();
                if (take('}')) return map;
                do {
                    space();
                    if (at >= text.length() || text.charAt(at) != '"') throw error();
                    String key = string();
                    space();
                    if (!take(':')) throw error();
                    map.put(key, value(depth + 1));
                    space();
                } while (take(','));
                if (!take('}')) throw error();
                return map;
            }
            if (c == '[') {
                at++;
                List<Object> list = new ArrayList<>();
                space();
                if (take(']')) return list;
                do {
                    list.add(value(depth + 1));
                    space();
                } while (take(','));
                if (!take(']')) throw error();
                return list;
            }
            if (c == '"') return string();
            if (text.startsWith("true", at)) { at += 4; return Boolean.TRUE; }
            if (text.startsWith("false", at)) { at += 5; return Boolean.FALSE; }
            if (text.startsWith("null", at)) { at += 4; return null; }
            int start = at;
            while (at < text.length() && "+-0123456789.eE".indexOf(text.charAt(at)) >= 0) at++;
            try {
                return Double.valueOf(text.substring(start, at));
            } catch (NumberFormatException e) {
                throw error();
            }
        }

        private String string() {
            at++;  // the opening quote
            StringBuilder out = new StringBuilder();
            while (at < text.length()) {
                char c = text.charAt(at++);
                if (c == '"') return out.toString();
                if (c != '\\') {
                    out.append(c);
                    continue;
                }
                if (at >= text.length()) break;
                char e = text.charAt(at++);
                switch (e) {
                    case 'b': out.append('\b'); break;
                    case 'f': out.append('\f'); break;
                    case 'n': out.append('\n'); break;
                    case 'r': out.append('\r'); break;
                    case 't': out.append('\t'); break;
                    case 'u':
                        if (at + 4 > text.length()) throw error();
                        try {
                            out.append((char) Integer.parseInt(text.substring(at, at + 4), 16));
                        } catch (NumberFormatException bad) {
                            throw error();
                        }
                        at += 4;
                        break;
                    default: out.append(e);
                }
            }
            throw error();
        }

        private void space() {
            while (at < text.length() && " \t\r\n".indexOf(text.charAt(at)) >= 0) at++;
        }

        private boolean take(char c) {
            if (at < text.length() && text.charAt(at) == c) {
                at++;
                return true;
            }
            return false;
        }

        private IllegalArgumentException error() {
            return new IllegalArgumentException("JSON syntax at " + at);
        }
    }
}
