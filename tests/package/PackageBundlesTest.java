/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.app;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

/**
 * The production bundle classification and APK set selection on real ZIP files; no APK
 * parser or device. The APK set's toc.pb is written here in protobuf wire format, with the
 * field numbers of bundletool's commands.proto and targeting.proto, laid out as bundletool
 * build-apks lays out a set: a standalone variant per ABI below SDK 21, split variants for
 * SDK 21 and SDK 31, an instant variant, ABI/density/language config splits, an install-time
 * and an on-demand feature module, and install-time and on-demand asset packs.
 */
public final class PackageBundlesTest {
    private static int checks;
    private static void check(boolean ok, String message) {
        if (!ok) throw new AssertionError(message);
        checks++;
    }
    private static File zip(File dir, String name, Map<String, byte[]> entries) throws IOException {
        File file = new File(dir, name);
        try (ZipOutputStream out = new ZipOutputStream(Files.newOutputStream(file.toPath()))) {
            for (Map.Entry<String, byte[]> entry : entries.entrySet()) {
                out.putNextEntry(new ZipEntry(entry.getKey()));
                out.write(entry.getValue());
                out.closeEntry();
            }
        }
        return file;
    }
    private static Map<String, byte[]> entries(String... names) {
        Map<String, byte[]> map = new LinkedHashMap<>();
        for (String name : names) map.put(name, name.endsWith("/") ? new byte[0] : new byte[] {7});
        return map;
    }

    public static void main(String[] args) throws Exception {
        File work = new File(args[0]);
        Files.createDirectories(work.toPath());
        detection(work);
        apkSet(work);
        universalSet(work);
        System.out.println("PackageBundlesTest all passed: " + checks + " checks");
    }

    private static void detection(File work) throws Exception {
        Map<String, byte[]> apk = entries("AndroidManifest.xml", "classes.dex", "assets/inner.apk");
        check(!PackageBundles.isBundle(zip(work, "plain.apk", apk)), "An APK is not a bundle");
        check(!PackageBundles.isBundle(zip(work, "plain.xapk", apk)),
                "An APK saved as .xapk was classified by its suffix");
        check(!PackageBundles.isBundle(zip(work, "plain.apks", apk)),
                "An APK saved as .apks was classified by its suffix");
        check(PackageBundles.isBundle(zip(work, "set.apk", entries("base.apk", "split_config.xxhdpi.apk"))),
                "A ZIP of APKs saved as .apk was not recognized as a bundle");
        check(PackageBundles.isBundle(zip(work, "set.apkm", entries("info.json", "base.apk"))),
                "An APKM was not recognized as a bundle");
        check(PackageBundles.isBundle(zip(work, "set.apks", entries("toc.pb", "splits/base-master.apk"))),
                "A bundletool APK set was not recognized as a bundle");
        check(!PackageBundles.isBundle(zip(work, "empty.xapk", entries("manifest.json", "dir.apk/"))),
                "A ZIP without APK files was classified as a bundle");
        File notZip = new File(work, "garbage.apkm");
        Files.write(notZip.toPath(), new byte[] {1, 2, 3});
        check(!PackageBundles.isBundle(notZip), "A file that is not a ZIP was classified as a bundle");
    }

    /** A protobuf message under construction, in wire format. */
    private static final class P {
        final ByteArrayOutputStream out = new ByteArrayOutputStream();
        private void varint(long value) {
            while ((value & ~0x7fL) != 0) { out.write((int) ((value & 0x7f) | 0x80)); value >>>= 7; }
            out.write((int) value);
        }
        P v(int field, long value) { varint(field << 3); varint(value); return this; }
        P b(int field, byte[] bytes) {
            varint((field << 3) | 2);
            varint(bytes.length);
            out.writeBytes(bytes);
            return this;
        }
        P s(int field, String value) { return b(field, value.getBytes(StandardCharsets.UTF_8)); }
        P m(int field, P message) { return b(field, message.out.toByteArray()); }
    }
    private static final int ARMEABI_V7A = 2, ARM64 = 3, X86 = 4, X86_64 = 5;
    private static final int MDPI = 3, XHDPI = 6, XXHDPI = 7, XXXHDPI = 8;
    private static P sdk(int min, int... alternatives) {
        P t = new P().m(1, new P().m(1, new P().v(1, min)));
        for (int alternative : alternatives) t.m(2, new P().m(1, new P().v(1, alternative)));
        return t;
    }
    private static P abi(int value, int... alternatives) {
        P t = new P().m(1, new P().v(1, value));
        for (int alternative : alternatives) t.m(2, new P().v(1, alternative));
        return t;
    }
    private static P density(int value, int... alternatives) {
        P t = new P().m(1, new P().v(1, value));
        for (int alternative : alternatives) t.m(2, new P().v(1, alternative));
        return t;
    }
    private static P language(String[] values, String... alternatives) {
        P t = new P();
        for (String value : values) t.s(1, value);
        for (String alternative : alternatives) t.s(2, alternative);
        return t;
    }
    /** Every APK path the fixture's toc.pb names, for its ZIP. */
    private static final List<String> NAMED = new ArrayList<>();
    /** ApkDescription: targeting, path, split_apk_metadata (or instant_apk_metadata). */
    private static P apk(String path, P targeting, boolean instant) {
        NAMED.add(path);
        return new P().m(1, targeting).s(2, path).m(instant ? 5 : 3, new P().s(1, "x"));
    }
    private static P module(String name, int delivery, String... dependencies) {
        P m = new P().s(1, name).v(6, delivery);
        for (String dependency : dependencies) m.s(4, dependency);
        return m;
    }
    private static final int[] OTHER_ABIS = {ARMEABI_V7A, ARM64, X86, X86_64};
    private static int[] except(int[] all, int value) {
        return Arrays.stream(all).filter(x -> x != value).toArray();
    }
    /** A split variant: SDK targeting and the suffix bundletool gives a later variant's files. */
    private static P splitVariant(P sdk, String suffix) {
        P base = new P().m(1, module("base", 1));
        String[] abiNames = {null, null, "armeabi_v7a", "arm64_v8a", "x86", "x86_64"};
        base.m(2, apk("splits/base-master" + suffix + ".apk", new P(), false));
        for (int abi : OTHER_ABIS) {
            base.m(2, apk("splits/base-" + abiNames[abi] + suffix + ".apk",
                    new P().m(1, abi(abi, except(OTHER_ABIS, abi))), false));
        }
        int[] densities = {MDPI, XHDPI, XXHDPI, XXXHDPI};
        String[] densityNames = {null, null, null, "mdpi", null, null, "xhdpi", "xxhdpi", "xxxhdpi"};
        for (int d : densities) {
            base.m(2, apk("splits/base-" + densityNames[d] + suffix + ".apk",
                    new P().m(4, density(d, except(densities, d))), false));
        }
        base.m(2, apk("splits/base-en" + suffix + ".apk", new P().m(3, language(new String[] {"en"}, "iw")), false));
        base.m(2, apk("splits/base-iw" + suffix + ".apk", new P().m(3, language(new String[] {"iw"}, "en")), false));
        base.m(2, apk("splits/base-other_lang" + suffix + ".apk",
                new P().m(3, language(new String[0], "en", "iw")), false));
        P camera = new P().m(1, module("camera", 1, "vision"));
        camera.m(2, apk("splits/camera-master" + suffix + ".apk", new P(), false));
        camera.m(2, apk("splits/camera-x86_64" + suffix + ".apk", new P().m(1, abi(X86_64, ARM64)), false));
        camera.m(2, apk("splits/camera-arm64_v8a" + suffix + ".apk", new P().m(1, abi(ARM64, X86_64)), false));
        P vision = new P().m(1, module("vision", 2));
        vision.m(2, apk("splits/vision-master" + suffix + ".apk", new P(), false));
        P extra = new P().m(1, module("extra", 2));
        extra.m(2, apk("splits/extra-master" + suffix + ".apk", new P(), false));
        P legacy = new P().m(1, new P().s(1, "legacy").v(2, 1));  // pre-0.10.2: on-demand flag only
        legacy.m(2, apk("splits/legacy-master" + suffix + ".apk", new P(), false));
        P old = new P().m(1, new P().s(1, "old"));  // pre-0.10.2: install-time
        old.m(2, apk("splits/old-master" + suffix + ".apk", new P(), false));
        return new P().m(1, new P().m(1, sdk)).m(2, base).m(2, camera).m(2, vision).m(2, extra)
                .m(2, legacy).m(2, old);
    }
    private static File apkSetFile(File work, String name, byte[] toc, List<String> names) throws IOException {
        Map<String, byte[]> files = new LinkedHashMap<>();
        files.put("toc.pb", toc);
        for (String path : names) files.put(path, new byte[] {7});
        return zip(work, name, files);
    }
    private static List<String> select(File set, PackageBundles.Device device) throws IOException {
        try (ZipFile zip = new ZipFile(set)) {
            return PackageBundles.select(zip, device);
        }
    }
    private static void expect(List<String> actual, String message, String... expected) {
        check(actual.equals(Arrays.asList(expected)), message + ": " + actual);
    }
    private static void rejects(File set, PackageBundles.Device device, String reason, String message)
            throws Exception {
        try {
            select(set, device);
        } catch (IOException expected) {
            check(expected.getMessage().contains(reason), message + ": " + expected.getMessage());
            return;
        }
        throw new AssertionError(message);
    }

    private static void apkSet(File work) throws Exception {
        NAMED.clear();
        P result = new P();
        // Standalone variants below SDK 21, one per ABI (bundletool's standalones/).
        for (int abi : new int[] {ARM64, X86_64, X86}) {
            String file = "standalones/standalone-"
                    + new String[] {null, null, null, "arm64_v8a", "x86", "x86_64"}[abi] + ".apk";
            P set = new P().m(1, module("base", 1)).m(2, apk(file, new P().m(1, abi(abi)), false));
            P sdkAndAbi = new P().m(1, sdk(15, 21, 31)).m(2, abi(abi, except(new int[] {ARM64, X86_64, X86}, abi)));
            result.m(1, new P().m(1, sdkAndAbi).m(2, set).v(3, NAMED.size()));
        }
        result.m(1, splitVariant(sdk(21, 15, 31), ""));
        result.m(1, splitVariant(sdk(31, 15, 21), "_2"));
        // An instant variant is never what an install gets.
        P instant = new P().m(1, module("base", 1)).m(2, apk("instant/instant-base-master.apk", new P(), true));
        result.m(1, new P().m(1, new P().m(1, sdk(31, 15, 21))).m(2, instant));
        // Asset packs: install-time (delivery 1) and on-demand (2).
        result.m(3, new P().m(1, new P().s(1, "textures").v(4, 1))
                .m(2, apk("asset-slices/textures-master.apk", new P(), false)));
        result.m(3, new P().m(1, new P().s(1, "levels").v(4, 2))
                .m(2, apk("asset-slices/levels-master.apk", new P(), false)));
        result.s(4, "fixture.apkset");
        byte[] toc = result.out.toByteArray();
        List<String> names = new ArrayList<>(NAMED);
        File set = apkSetFile(work, "set.apks", toc, names);
        check(PackageBundles.isBundle(set), "A bundletool APK set is a bundle");

        expect(select(set, PackageBundles.Device.gesher(36)),
                "Gesher's device: the SDK 31 variant, x86_64, every density and language split",
                "splits/base-master_2.apk", "splits/base-x86_64_2.apk",
                "splits/base-mdpi_2.apk", "splits/base-xhdpi_2.apk", "splits/base-xxhdpi_2.apk",
                "splits/base-xxxhdpi_2.apk", "splits/base-en_2.apk", "splits/base-iw_2.apk",
                "splits/base-other_lang_2.apk", "splits/camera-master_2.apk", "splits/camera-x86_64_2.apk",
                "splits/vision-master_2.apk", "splits/old-master_2.apk", "asset-slices/textures-master.apk");
        expect(select(set, new PackageBundles.Device(36, new String[] {"arm64-v8a"}, 420, new String[] {"he-IL"})),
                "A full device spec: arm64-v8a, xxhdpi for 420 dpi, Hebrew without the fallback",
                "splits/base-master_2.apk", "splits/base-arm64_v8a_2.apk", "splits/base-xxhdpi_2.apk",
                "splits/base-iw_2.apk", "splits/camera-master_2.apk", "splits/camera-arm64_v8a_2.apk",
                "splits/vision-master_2.apk", "splits/old-master_2.apk", "asset-slices/textures-master.apk");
        expect(select(set, new PackageBundles.Device(28, new String[] {"x86_64"}, 160,
                        new String[] {"fr-FR", "en-US"})),
                "SDK 28: the SDK 21 variant, mdpi, English and the fallback for French",
                "splits/base-master.apk", "splits/base-x86_64.apk", "splits/base-mdpi.apk",
                "splits/base-en.apk", "splits/base-other_lang.apk", "splits/camera-master.apk",
                "splits/camera-x86_64.apk", "splits/vision-master.apk", "splits/old-master.apk",
                "asset-slices/textures-master.apk");
        expect(select(set, new PackageBundles.Device(19, new String[] {"x86_64", "arm64-v8a"}, 0, null)),
                "Below SDK 21: the standalone APK of the preferred ABI",
                "standalones/standalone-x86_64.apk", "asset-slices/textures-master.apk");
        rejects(set, new PackageBundles.Device(36, new String[] {"armeabi-v7a", "riscv64"}, 0, null),
                "INSTALL_FAILED_NO_MATCHING_ABIS", "A device whose ABIs the set lacks got APKs");
        rejects(set, new PackageBundles.Device(10, new String[] {"x86_64"}, 0, null),
                "INSTALL_FAILED_OLDER_SDK", "A device below every variant's SDK got APKs");
        // Densities as Android's resource choice picks them (ResTable_config::isBetterThan).
        String[] expectedDensity = {"mdpi", "mdpi", "xhdpi", "xhdpi", "xxhdpi", "xxhdpi", "xxxhdpi", "xxxhdpi"};
        int[] dpis = {120, 160, 240, 320, 400, 480, 560, 640};
        for (int i = 0; i < dpis.length; i++) {
            List<String> picked = select(set, new PackageBundles.Device(36, new String[] {"x86_64"}, dpis[i], null));
            check(picked.contains("splits/base-" + expectedDensity[i] + "_2.apk")
                    && picked.stream().filter(p -> p.matches("splits/base-x*[hm]dpi_2\\.apk")).count() == 1,
                    "density " + dpis[i] + " picked " + picked);
        }

        List<String> incomplete = new ArrayList<>(names);
        incomplete.remove("splits/base-master_2.apk");
        File missing = apkSetFile(work, "missing.apks", toc, incomplete);
        rejects(missing, PackageBundles.Device.gesher(36), "does not hold", "A toc naming a missing APK was accepted");
        File truncated = apkSetFile(work, "truncated.apks", Arrays.copyOf(toc, toc.length - 3), names);
        rejects(truncated, PackageBundles.Device.gesher(36), "toc.pb", "A truncated toc.pb was accepted");
        P tiered = new P().m(1, new P().m(1, new P().m(1, sdk(21)))
                .m(2, new P().m(1, module("base", 1))
                        .m(2, apk("splits/base-master.apk", new P(), false))
                        .m(2, apk("splits/base-tier_1.apk", new P().m(9, new P().m(3, new P().v(1, 1))), false))));
        File tiers = apkSetFile(work, "tiers.apks", tiered.out.toByteArray(),
                List.of("splits/base-master.apk", "splits/base-tier_1.apk"));
        rejects(tiers, PackageBundles.Device.gesher(36), "device tiers", "Device tier targeting was guessed");
    }

    /** build-apks --mode=universal: one standalone variant, universal.apk. */
    private static void universalSet(File work) throws Exception {
        P result = new P().m(1, new P().m(1, new P()).m(2, new P().m(1, module("base", 1))
                .m(2, new P().m(1, new P()).s(2, "universal.apk").m(4, new P().s(1, "base")))));
        File set = apkSetFile(work, "universal.apks", result.out.toByteArray(), List.of("universal.apk"));
        expect(select(set, PackageBundles.Device.gesher(36)), "A universal APK set installs universal.apk",
                "universal.apk");
        File plain = zip(work, "plain-set.xapk", entries("base.apk", "split_config.x86_64.apk", "icon.png"));
        try (ZipFile zip = new ZipFile(plain)) {
            expect(PackageBundles.select(zip, PackageBundles.Device.gesher(36)),
                    "Without toc.pb every APK entry is the install", "base.apk", "split_config.x86_64.apk");
        }
    }
}
