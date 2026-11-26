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
        deviceDefaults(work);
        deliveryTypes(work);
        limits(work);
        universalSet(work);
        xapkManifest(work);
        encrypted(work);
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
    }

    /** ApkDescription without recording it in NAMED. */
    private static P split(String path, P targeting) {
        return new P().m(1, targeting).s(2, path).m(3, new P().s(1, "x"));
    }
    /** google.protobuf.Int32Value: 0 is the empty message, as protobuf writes it. */
    private static P int32(int value) {
        return value == 0 ? new P() : new P().v(1, value);
    }
    /** BuildApksResult.default_targeting_value: SplitDimension.Value and its default. */
    private static P defaultValue(int dimension, String value) {
        return new P().v(1, dimension).s(2, value);
    }
    private static final int DEVICE_TIER = 6, COUNTRY_SET = 7, DEVICE_GROUP = 9;
    /** Three splits targeted at {@code values} in turn (each with the others as alternatives) and a fallback. */
    private static List<P> targeted(int field, String prefix, String... values) {
        List<P> splits = new ArrayList<>();
        for (String value : values) {
            P t = new P().s(1, value);
            for (String other : values) if (!other.equals(value)) t.s(2, other);
            splits.add(split("splits/base-" + prefix + "_" + value + ".apk", new P().m(field, t)));
        }
        P fallback = new P();
        for (String value : values) fallback.s(2, value);
        splits.add(split("splits/base-" + prefix + "_other.apk", new P().m(field, fallback)));
        return splits;
    }
    private static List<String> paths(String... paths) {
        return Arrays.asList(paths);
    }

    /**
     * Device tier, country set and device group: Gesher's device defines none, so the set's
     * own defaults apply, as bundletool extract-apks / install-apks fill them in
     * (ExtractApksCommand.applyDefaultsToDeviceSpec): tier 0, the fallback country split and
     * no group without defaults.
     */
    private static void deviceDefaults(File work) throws Exception {
        P base = new P().m(1, module("base", 1)).m(2, split("splits/base-master.apk", new P()));
        List<String> files = new ArrayList<>(List.of("splits/base-master.apk", "splits/hq-master.apk",
                "asset-slices/pack-tier_0.apk", "asset-slices/pack-tier_1.apk", "asset-slices/pack-tier_2.apk"));
        for (int tier = 0; tier < 3; tier++) {
            P t = new P().m(3, int32(tier));
            for (int other = 0; other < 3; other++) if (other != tier) t.m(4, int32(other));
            base.m(2, split("splits/base-tier_" + tier + ".apk", new P().m(9, t)));
            files.add("splits/base-tier_" + tier + ".apk");
        }
        for (P p : targeted(10, "countries", "latam", "sea")) base.m(2, p);
        for (P p : targeted(11, "group", "high", "low")) base.m(2, p);
        for (String name : new String[] {"countries_latam", "countries_sea", "countries_other",
                "group_high", "group_low", "group_other"}) {
            files.add("splits/base-" + name + ".apk");
        }
        // A conditional module for the "high" device group (ModuleTargeting.device_group_targeting).
        P hq = new P().m(1, module("hq", 1).m(5, new P().m(5, new P().s(1, "high"))))
                .m(2, split("splits/hq-master.apk", new P()));
        P pack = new P().m(1, new P().s(1, "pack").v(4, 1));
        for (int tier = 0; tier < 3; tier++) {
            P t = new P().m(3, int32(tier));
            for (int other = 0; other < 3; other++) if (other != tier) t.m(4, int32(other));
            pack.m(2, split("asset-slices/pack-tier_" + tier + ".apk", new P().m(9, t)));
        }
        byte[] variants = new P().m(1, new P().m(1, new P().m(1, sdk(21))).m(2, base).m(2, hq)).out.toByteArray();
        PackageBundles.Device gesher = PackageBundles.Device.gesher(36);

        P plain = new P();
        plain.out.writeBytes(variants);
        plain.m(3, pack);
        expect(select(apkSetFile(work, "no-defaults.apks", plain.out.toByteArray(), files), gesher),
                "Without defaults: tier 0, the fallback country split, no device group",
                "splits/base-master.apk", "splits/base-tier_0.apk", "splits/base-countries_other.apk",
                "asset-slices/pack-tier_0.apk");

        P named = new P();
        named.out.writeBytes(variants);
        named.m(3, pack).m(7, defaultValue(DEVICE_TIER, "2")).m(7, defaultValue(COUNTRY_SET, "latam"))
                .m(7, defaultValue(DEVICE_GROUP, "high"));
        expect(select(apkSetFile(work, "defaults.apks", named.out.toByteArray(), files), gesher),
                "The set's defaults: tier 2, latam, the high group and its conditional module",
                "splits/base-master.apk", "splits/base-tier_2.apk", "splits/base-countries_latam.apk",
                "splits/base-group_high.apk", "splits/hq-master.apk", "asset-slices/pack-tier_2.apk");

        P unknownTier = new P();
        unknownTier.out.writeBytes(variants);
        unknownTier.m(7, defaultValue(DEVICE_TIER, "5"));
        rejects(apkSetFile(work, "unknown-tier.apks", unknownTier.out.toByteArray(), files), gesher,
                "device tier 5", "A default tier no split targets was accepted");
        P badTier = new P();
        badTier.out.writeBytes(variants);
        badTier.m(7, defaultValue(DEVICE_TIER, "high"));
        rejects(apkSetFile(work, "bad-tier.apks", badTier.out.toByteArray(), files), gesher,
                "default device tier", "A default tier that is not a number was accepted");
    }

    /**
     * Module delivery as bundletool reads it by the set's version: delivery_type from 0.10.2
     * (an unknown one is not install-time), the deprecated on-demand flag before it.
     */
    private static void deliveryTypes(File work) throws Exception {
        List<String> files = List.of("splits/base-master.apk", "splits/unknown-master.apk",
                "splits/flagged-master.apk");
        for (String version : new String[] {"0.9.0", "1.18.3"}) {
            P variant = new P().m(1, new P().m(1, sdk(21)))
                    .m(2, new P().m(1, module("base", 1)).m(2, split("splits/base-master.apk", new P())))
                    .m(2, new P().m(1, new P().s(1, "unknown")).m(2, split("splits/unknown-master.apk", new P())))
                    .m(2, new P().m(1, new P().s(1, "flagged").v(2, 1).v(6, 1))
                            .m(2, split("splits/flagged-master.apk", new P())));
            P result = new P().m(1, variant).m(2, new P().s(2, version));
            List<String> picked = select(apkSetFile(work, "delivery-" + version + ".apks",
                    result.out.toByteArray(), files), PackageBundles.Device.gesher(36));
            if (version.equals("0.9.0")) {
                expect(picked, "bundletool 0.9.0: the on-demand flag decides",
                        "splits/base-master.apk", "splits/unknown-master.apk");
            } else {
                expect(picked, "bundletool 1.18.3: the delivery type decides",
                        "splits/base-master.apk", "splits/flagged-master.apk");
            }
        }
    }

    /** toc.pb and manifest.json are bounded in size, and toc.pb in fields decoded. */
    private static void limits(File work) throws Exception {
        PackageBundles.Device gesher = PackageBundles.Device.gesher(36);
        // About 600,000 empty variants in 1.2 MB, which deflates to a few kilobytes.
        P many = new P();
        byte[] empty = new P().b(1, new byte[0]).out.toByteArray();
        for (int i = 0; i < ApkSetToc.MAX_DECODED_FIELDS + 100_000; i++) many.out.writeBytes(empty);
        long start = System.nanoTime();
        rejects(apkSetFile(work, "complex.apks", many.out.toByteArray(), List.of()), gesher,
                "fields", "A toc.pb with more fields than the budget was decoded");
        check(System.nanoTime() - start < 10_000_000_000L, "Rejecting a complex toc.pb took too long");
        rejects(apkSetFile(work, "oversized.apks", new byte[PackageBundles.MAX_TOC + 1], List.of()), gesher,
                "too large", "An oversized toc.pb was read");
        Map<String, byte[]> big = entries("com.example.apk");
        byte[] manifest = new byte[PackageBundles.MAX_MANIFEST + 1];
        Arrays.fill(manifest, (byte) ' ');
        manifest[0] = '{';
        manifest[manifest.length - 1] = '}';
        big.put("manifest.json", manifest);
        try (ZipFile zip = new ZipFile(zip(work, "big-manifest.xapk", big))) {
            PackageBundles.select(zip, gesher);
            throw new AssertionError("An oversized manifest.json was read");
        } catch (IOException expected) {
            check(expected.getMessage().contains("too large"), "oversized manifest.json: " + expected.getMessage());
        }
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

    /** APKPure's XAPK: manifest.json lists the APKs to install and the expansion files. */
    private static void xapkManifest(File work) throws Exception {
        Map<String, byte[]> files = entries("com.example.apk", "config.arm64_v8a.apk", "config.xxhdpi.apk",
                "unlisted.apk", "icon.png", "Android/obb/com.example/main.7.com.example.obb");
        files.put("manifest.json", ("{\"xapk_version\": 2, \"package_name\": \"com.example\","
                + " \"split_configs\": [\"config.arm64_v8a\", \"config.xxhdpi\"], \"split_apks\": ["
                + "{\"file\": \"com.example.apk\", \"id\": \"base\"},"
                + "{\"file\": \"config.arm64_v8a.apk\", \"id\": \"config.arm64_v8a\"},"
                + "{\"file\": \"config.xxhdpi.apk\", \"id\": \"config.xxhdpi\"}],"
                + " \"expansions\": [{\"file\": \"Android/obb/com.example/main.7.com.example.obb\","
                + " \"install_location\": \"EXTERNAL_STORAGE\", \"install_path\":"
                + " \"Android/obb/com.example/main.7.com.example.obb\"}], \"name\": \"Ex\\u00e9mple \\\"q\\\"\"}")
                .getBytes(StandardCharsets.UTF_8));
        File xapk = zip(work, "listed.xapk", files);
        try (ZipFile zip = new ZipFile(xapk)) {
            expect(PackageBundles.select(zip, PackageBundles.Device.gesher(36)),
                    "manifest.json's split_apks are the install", "com.example.apk", "config.arm64_v8a.apk",
                    "config.xxhdpi.apk");
            expect(PackageBundles.expansions(zip), "The expansion file is reported once",
                    "Android/obb/com.example/main.7.com.example.obb");
        }
        Map<String, byte[]> single = entries("com.example.apk", "icon.png");
        single.put("manifest.json", "{\"xapk_version\": 1, \"package_name\": \"com.example\"}"
                .getBytes(StandardCharsets.UTF_8));
        try (ZipFile zip = new ZipFile(zip(work, "single.xapk", single))) {
            expect(PackageBundles.select(zip, PackageBundles.Device.gesher(36)),
                    "Without split_apks every APK entry is the install", "com.example.apk");
            check(PackageBundles.expansions(zip).isEmpty(), "No expansion files invented");
        }
        // Some tools write a UTF-8 byte order mark before the JSON.
        Map<String, byte[]> bom = entries("com.example.apk", "config.x86_64.apk", "unlisted.apk");
        bom.put("manifest.json", ("\uFEFF{\"package_name\": \"com.example\", \"split_apks\": ["
                + "{\"file\": \"com.example.apk\", \"id\": \"base\"},"
                + " {\"file\": \"config.x86_64.apk\", \"id\": \"config.x86_64\"}]}").getBytes(StandardCharsets.UTF_8));
        try (ZipFile zip = new ZipFile(zip(work, "bom.xapk", bom))) {
            expect(PackageBundles.select(zip, PackageBundles.Device.gesher(36)),
                    "A manifest.json after a byte order mark names the install", "com.example.apk",
                    "config.x86_64.apk");
        }
        Map<String, byte[]> missing = entries("com.example.apk");
        missing.put("manifest.json", "{\"split_apks\": [{\"file\": \"gone.apk\"}]}".getBytes(StandardCharsets.UTF_8));
        try (ZipFile zip = new ZipFile(zip(work, "missing.xapk", missing))) {
            PackageBundles.select(zip, PackageBundles.Device.gesher(36));
            throw new AssertionError("manifest.json naming a missing APK was accepted");
        } catch (IOException expected) {
            check(expected.getMessage().contains("gone.apk"), "missing listed APK named: " + expected.getMessage());
        }
        Map<String, byte[]> broken = entries("com.example.apk");
        broken.put("manifest.json", "{\"split_apks\": [".getBytes(StandardCharsets.UTF_8));
        try (ZipFile zip = new ZipFile(zip(work, "broken.xapk", broken))) {
            PackageBundles.select(zip, PackageBundles.Device.gesher(36));
            throw new AssertionError("A broken manifest.json was ignored");
        } catch (IOException expected) {
            check(expected.getMessage().contains("manifest.json"), "broken manifest.json reported");
        }
    }

    /** An encrypted ZIP (as APKMirror's encrypted .apkm) is neither a bundle nor an APK ZIP. */
    private static void encrypted(File work) throws Exception {
        File file = zip(work, "encrypted.apkm", entries("info.json", "base.apk"));
        check(PackageBundles.isZip(file), "The fixture is a ZIP before its entries are marked encrypted");
        byte[] bytes = Files.readAllBytes(file.toPath());
        // Set bit 0 (encrypted) of every local and central header's general purpose flags.
        for (int i = 0; i + 4 <= bytes.length; i++) {
            int signature = (bytes[i] & 0xff) | (bytes[i + 1] & 0xff) << 8 | (bytes[i + 2] & 0xff) << 16
                    | (bytes[i + 3] & 0xff) << 24;
            if (signature == 0x04034b50) bytes[i + 6] |= 1;
            if (signature == 0x02014b50) bytes[i + 8] |= 1;
        }
        Files.write(file.toPath(), bytes);
        check(!PackageBundles.isZip(file), "An encrypted ZIP was readable");
        check(!PackageBundles.isBundle(file), "An encrypted ZIP was classified as a bundle");
    }
}
