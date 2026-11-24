/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/**
 * The APKs of a bundletool APK set ({@code .apks}) that a device gets, read from the set's
 * {@code toc.pb} (a {@code BuildApksResult}, bundletool's {@code commands.proto}) as
 * {@code bundletool install-apks} and {@code extract-apks} choose them: one variant
 * ({@code VariantMatcher}), its install-time modules with their dependencies, the APKs of
 * those modules whose targeting matches ({@code ApkMatcher}), and the install-time asset
 * slices. Each targeting dimension follows bundletool's matcher of that name
 * (github.com/google/bundletool, {@code com.android.tools.build.bundletool.device}), with
 * the device tier, country set and device group the set names as its defaults, as
 * {@code extract-apks} fills them into a device spec that has none
 * ({@code ExtractApksCommand.applyDefaultsToDeviceSpec}).
 *
 * <p>The protobuf wire format is read directly: the fields below are those of bundletool's
 * {@code commands.proto} and {@code targeting.proto}.
 */
final class ApkSetToc {
    private ApkSetToc() {}

    // BuildApksResult, Bundletool, DefaultTargetingValue, SplitDimension.Value
    private static final int RESULT_VARIANT = 1, RESULT_BUNDLETOOL = 2, RESULT_ASSET_SLICE_SET = 3,
            RESULT_DEFAULT_TARGETING = 7;
    private static final int BUNDLETOOL_VERSION = 2, DEFAULT_DIMENSION = 1, DEFAULT_VALUE = 2;
    private static final int DIMENSION_DEVICE_TIER = 6, DIMENSION_COUNTRY_SET = 7, DIMENSION_DEVICE_GROUP = 9;
    // Variant, VariantTargeting
    private static final int VARIANT_TARGETING = 1, VARIANT_APK_SET = 2;
    private static final int VT_SDK = 1, VT_ABI = 2, VT_DENSITY = 3, VT_MULTI_ABI = 4, VT_SDK_RUNTIME = 6;
    // ApkSet, ModuleMetadata, ModuleTargeting
    private static final int SET_MODULE = 1, SET_APK = 2;
    private static final int MODULE_NAME = 1, MODULE_ON_DEMAND_DEPRECATED = 2, MODULE_DEPENDENCIES = 4,
            MODULE_TARGETING = 5, MODULE_DELIVERY_TYPE = 6;
    private static final int MT_SDK = 1, MT_DEVICE_GROUP = 5;
    // AssetSliceSet, AssetModuleMetadata
    private static final int SLICES_MODULE = 1, SLICES_APK = 2, ASSET_MODULE_NAME = 1, ASSET_DELIVERY_TYPE = 4;
    // ApkDescription, ApkTargeting
    private static final int APK_TARGETING = 1, APK_PATH = 2, APK_INSTANT = 5;
    private static final int AT_ABI = 1, AT_LANGUAGE = 3, AT_DENSITY = 4, AT_SDK = 5, AT_TCF = 6,
            AT_MULTI_ABI = 7, AT_DEVICE_TIER = 9, AT_COUNTRY_SET = 10, AT_DEVICE_GROUP = 11;
    // Every *Targeting message: repeated value = 1, alternatives = 2 (DeviceTierTargeting: 3, 4).
    private static final int VALUE = 1, ALTERNATIVES = 2, TIER_VALUE = 3, TIER_ALTERNATIVES = 4;
    private static final int INSTALL_TIME = 1;  // DeliveryType.INSTALL_TIME

    /** Abi.AbiAlias in enum order, which is also bundletool's ARCHITECTURE_ORDERING. */
    private static final String[] ABI_ALIASES = {null, "armeabi", "armeabi-v7a", "arm64-v8a", "x86",
            "x86_64", "mips", "mips64", "riscv64"};
    /** ScreenDensity.DensityAlias to dpi (bundletool's ResourcesUtils.DENSITY_ALIAS_TO_DPI_MAP). */
    private static final int[] DENSITY_DPI = {0, 0xffff, 120, 160, 213, 240, 320, 480, 640};
    private static final int DEFAULT_DENSITY = 0, ANY_DENSITY = 0xfffe, MDPI = 160;

    /**
     * The most protobuf fields read from one toc.pb, counting those of every nested message
     * read. Selection reads the chosen variant's modules and APK targeting: tens of thousands
     * of fields for a set of 60 modules with every ABI, density and 90 language splits in 4
     * variants. The cap keeps a small, highly compressible archive from making the installer
     * build millions of objects.
     */
    static final int MAX_DECODED_FIELDS = 500_000;

    /**
     * Bundletool's device spec beyond {@link PackageBundles.Device}: Gesher's device defines
     * no device tier, country set or device group, so these are the set's own defaults, or
     * tier 0 and no country set or group, as {@code extract-apks} fills them in.
     */
    private static final class Spec {
        final PackageBundles.Device device;
        final int tier;
        final String countrySet;
        final String group;
        /**
         * Whether modules carry delivery types (bundletool 0.10.2 and later) or only the
         * deprecated on-demand flag; null when the set names no readable version.
         */
        final Boolean deliveryTypes;

        Spec(PackageBundles.Device device, Msg result) throws IOException {
            this.device = device;
            int tier = 0;
            String countrySet = "", group = "";
            for (Msg value : result.messages(RESULT_DEFAULT_TARGETING)) {
                String text = value.string(DEFAULT_VALUE);
                if (text.isEmpty()) continue;
                long dimension = value.varint(DEFAULT_DIMENSION);
                if (dimension == DIMENSION_DEVICE_TIER) {
                    try {
                        tier = Integer.parseInt(text);
                    } catch (NumberFormatException e) {
                        throw new IOException("Invalid toc.pb: default device tier " + text);
                    }
                } else if (dimension == DIMENSION_COUNTRY_SET) {
                    countrySet = text;
                } else if (dimension == DIMENSION_DEVICE_GROUP) {
                    group = text;
                }
            }
            this.tier = tier;
            this.countrySet = countrySet;
            this.group = group;
            int[] version = version(result.message(RESULT_BUNDLETOOL).string(BUNDLETOOL_VERSION));
            deliveryTypes = version == null ? null : compareVersions(version, new int[] {0, 10, 2}) >= 0;
        }
    }

    /** The paths, inside the APK set, of the APKs {@code device} gets. */
    static List<String> select(byte[] toc, PackageBundles.Device device) throws IOException {
        try {
            Msg result = Msg.parse(toc, new int[] {MAX_DECODED_FIELDS});
            Spec spec = new Spec(device, result);
            List<String> paths = new ArrayList<>();
            Msg variant = variant(result.messages(RESULT_VARIANT), spec);
            Set<String> modules = installTimeModules(variant, spec);
            for (Msg set : variant.messages(VARIANT_APK_SET)) {
                if (!modules.contains(set.message(SET_MODULE).string(MODULE_NAME))) continue;
                for (Msg apk : set.messages(SET_APK)) {
                    checkCompatible(apk.message(APK_TARGETING), spec);
                    if (matches(apk.message(APK_TARGETING), spec)) paths.add(apk.string(APK_PATH));
                }
            }
            for (Msg slices : result.messages(RESULT_ASSET_SLICE_SET)) {
                // Asset packs: install-time ones only (ApkMatcher.getUpfrontAssetModules).
                if (slices.message(SLICES_MODULE).varint(ASSET_DELIVERY_TYPE) != INSTALL_TIME) continue;
                for (Msg apk : slices.messages(SLICES_APK)) {
                    if (matches(apk.message(APK_TARGETING), spec)) paths.add(apk.string(APK_PATH));
                }
            }
            return paths;
        } catch (UncheckedIOException invalid) {
            throw invalid.getCause();
        }
    }

    /** The numeric components of a bundletool version ("1.18.3", "0.10.2-dev"), or null. */
    private static int[] version(String text) {
        String[] parts = text.split("[.-]");
        if (parts.length < 3) return null;
        int[] version = new int[3];
        for (int i = 0; i < 3; i++) {
            try {
                version[i] = Integer.parseInt(parts[i]);
            } catch (NumberFormatException e) {
                return null;
            }
        }
        return version;
    }

    private static int compareVersions(int[] a, int[] b) {
        for (int i = 0; i < a.length; i++) {
            if (a[i] != b[i]) return Integer.compare(a[i], b[i]);
        }
        return 0;
    }

    /**
     * The one variant that matches (VariantMatcher.getMatchingVariant): not an instant one,
     * not one that requires the SDK runtime (Gesher has none), and matching every dimension
     * the device defines.
     */
    private static Msg variant(List<Msg> variants, Spec spec) throws IOException {
        PackageBundles.Device device = spec.device;
        List<Msg> matching = new ArrayList<>();
        for (Msg variant : variants) {
            if (isInstant(variant)) continue;
            Msg targeting = variant.message(VARIANT_TARGETING);
            if (targeting.message(VT_SDK_RUNTIME).varint(1) != 0) continue;
            checkSdkCompatible(targeting.message(VT_SDK), device);
            checkAbiCompatible(targeting.message(VT_ABI), device);
            if (sdkMatches(targeting.message(VT_SDK), device)
                    && abiMatches(targeting.message(VT_ABI), device)
                    && multiAbiMatches(targeting.message(VT_MULTI_ABI), device)
                    && (device.densityDpi == 0 || densityMatches(targeting.message(VT_DENSITY), device))) {
                // Texture compression formats: Gesher's device lists none, and a dimension
                // the device spec lacks matches any variant (getVariantTargetingPredicate).
                matching.add(variant);
            }
        }
        if (matching.isEmpty()) {
            throw new IOException("No variant of the APK set matches this device (" + device + ")");
        }
        if (matching.size() > 1) {
            throw new IOException("More than one variant of the APK set matches this device (" + device + ")");
        }
        return matching.get(0);
    }

    /** Every APK instant (VariantMatcher.isVariantInstant): so is a variant without APKs. */
    private static boolean isInstant(Msg variant) {
        for (Msg set : variant.messages(VARIANT_APK_SET)) {
            for (Msg apk : set.messages(SET_APK)) {
                if (!apk.has(APK_INSTANT)) return false;
            }
        }
        return true;
    }

    /**
     * The install-time modules whose conditions the device meets, with every module they
     * depend on ({@code ApkMatcher.getInstallTimeAndRequestedModulesWithDependencies}; every
     * module depends on base). A set from bundletool before 0.10.2 has no delivery type and
     * marks on-demand modules with the deprecated flag instead. A module conditioned on device
     * groups is installed for the set's default group only. Device features and OpenGL
     * versions are conditions Gesher's device spec does not list, so, as bundletool does for
     * a spec without them, they do not exclude a module.
     */
    private static Set<String> installTimeModules(Msg variant, Spec spec) {
        PackageBundles.Device device = spec.device;
        Map<String, List<String>> dependencies = new HashMap<>();
        Set<String> modules = new LinkedHashSet<>();
        for (Msg set : variant.messages(VARIANT_APK_SET)) {
            Msg module = set.message(SET_MODULE);
            String name = module.string(MODULE_NAME);
            List<String> needs = new ArrayList<>(module.strings(MODULE_DEPENDENCIES));
            needs.add("base");
            dependencies.put(name, needs);
            long delivery = module.varint(MODULE_DELIVERY_TYPE);
            // As bundletool by the set's version; without one, a delivery type where there is one.
            boolean types = spec.deliveryTypes != null ? spec.deliveryTypes : delivery != 0;
            boolean installTime = types ? delivery == INSTALL_TIME
                    : module.varint(MODULE_ON_DEMAND_DEPRECATED) == 0;
            Msg targeting = module.message(MODULE_TARGETING);
            if (installTime && sdkMatches(targeting.message(MT_SDK), device)
                    && groupMatches(targeting.message(MT_DEVICE_GROUP), spec)) {
                modules.add(name);
            }
        }
        List<String> pending = new ArrayList<>(modules);
        while (!pending.isEmpty()) {
            List<String> needs = dependencies.get(pending.remove(pending.size() - 1));
            if (needs == null) continue;
            for (String need : needs) if (modules.add(need)) pending.add(need);
        }
        return modules;
    }

    /** ApkMatcher.matchesApkTargeting over the dimensions an APK can be targeted by. */
    private static boolean matches(Msg t, Spec spec) {
        PackageBundles.Device device = spec.device;
        return sdkMatches(t.message(AT_SDK), device)
                && abiMatches(t.message(AT_ABI), device)
                && multiAbiMatches(t.message(AT_MULTI_ABI), device)
                && (device.densityDpi == 0 || densityMatches(t.message(AT_DENSITY), device))
                && (device.languages == null || languageMatches(t.message(AT_LANGUAGE), device))
                && tcfMatches(t.message(AT_TCF))
                && groupMatches(t.message(AT_DEVICE_GROUP), spec)
                && tierMatches(t.message(AT_DEVICE_TIER), spec)
                && countrySetMatches(t.message(AT_COUNTRY_SET), spec);
    }

    private static void checkCompatible(Msg t, Spec spec) throws IOException {
        checkSdkCompatible(t.message(AT_SDK), spec.device);
        checkAbiCompatible(t.message(AT_ABI), spec.device);
        checkTierCompatible(t.message(AT_DEVICE_TIER), spec);
        checkCountrySetCompatible(t.message(AT_COUNTRY_SET), spec);
    }

    // --- SdkVersionMatcher ---------------------------------------------------------------

    /** SdkVersion.min, or null when the value has no minimum (matches every device). */
    private static Integer min(Msg sdkVersion) {
        return sdkVersion.has(1) ? (int) sdkVersion.message(1).varint(1) : null;
    }

    private static boolean sdkFits(Msg sdkVersion, PackageBundles.Device device) {
        Integer min = min(sdkVersion);
        return min == null || min <= device.sdk;
    }

    private static boolean sdkMatches(Msg t, PackageBundles.Device device) {
        List<Msg> values = t.messages(VALUE);
        Msg value = values.isEmpty() ? Msg.EMPTY : values.get(0);
        if (!sdkFits(value, device)) return false;
        Integer own = min(value);
        for (Msg alternative : t.messages(ALTERNATIVES)) {
            Integer other = min(alternative);
            // A better match: fits the device and needs a higher SDK than the value.
            if (other != null && sdkFits(alternative, device) && other > (own == null ? 0 : own)) return false;
        }
        return true;
    }

    private static void checkSdkCompatible(Msg t, PackageBundles.Device device) throws IOException {
        List<Msg> all = new ArrayList<>(t.messages(VALUE));
        if (all.isEmpty()) all.add(Msg.EMPTY);
        all.addAll(t.messages(ALTERNATIVES));
        for (Msg value : all) if (sdkFits(value, device)) return;
        throw new IOException("INSTALL_FAILED_OLDER_SDK: the APK set needs a newer SDK than " + device.sdk);
    }

    // --- AbiMatcher, MultiAbiMatcher -----------------------------------------------------

    private static Set<String> abis(List<Msg> abis) {
        Set<String> names = new LinkedHashSet<>();
        for (Msg abi : abis) {
            int alias = (int) abi.varint(1);
            names.add(alias > 0 && alias < ABI_ALIASES.length ? ABI_ALIASES[alias] : "abi#" + alias);
        }
        return names;
    }

    private static boolean abiMatches(Msg t, PackageBundles.Device device) {
        if (t.isEmpty()) return true;
        Set<String> values = abis(t.messages(VALUE));
        Set<String> alternatives = abis(t.messages(ALTERNATIVES));
        // In the device's order of preference, the first one listed decides.
        for (String abi : device.abis) {
            if (values.contains(abi)) return true;
            if (alternatives.contains(abi)) return false;
        }
        return values.isEmpty();
    }

    private static void checkAbiCompatible(Msg t, PackageBundles.Device device) throws IOException {
        if (t.isEmpty()) return;
        Set<String> all = abis(t.messages(VALUE));
        all.addAll(abis(t.messages(ALTERNATIVES)));
        for (String abi : device.abis) if (all.contains(abi)) return;
        throw new IOException("INSTALL_FAILED_NO_MATCHING_ABIS: the APK set has native code only for "
                + String.join(", ", all) + "; Gesher runs " + String.join(" and ", device.abis));
    }

    private static boolean multiAbiMatches(Msg t, PackageBundles.Device device) {
        if (t.isEmpty()) return true;
        Set<String> deviceAbis = new HashSet<>(device.abis);
        List<List<Integer>> values = new ArrayList<>();
        boolean fits = false;
        for (Msg value : t.messages(VALUE)) {
            Set<String> set = abis(value.messages(1));
            fits |= deviceAbis.containsAll(set);
            values.add(sortedAliases(value.messages(1)));
        }
        if (!fits) return false;
        // No better alternative: one the device runs that ranks above every value.
        for (Msg alternative : t.messages(ALTERNATIVES)) {
            if (!deviceAbis.containsAll(abis(alternative.messages(1)))) continue;
            List<Integer> other = sortedAliases(alternative.messages(1));
            boolean better = true;
            for (List<Integer> value : values) better &= compareMultiAbi(other, value) > 0;
            if (better) return false;
        }
        return true;
    }

    /** The aliases from most to least preferable (TargetingComparators.sortMultiAbi). */
    private static List<Integer> sortedAliases(List<Msg> abis) {
        List<Integer> aliases = new ArrayList<>();
        for (Msg abi : abis) aliases.add((int) abi.varint(1));
        aliases.sort(Collections.reverseOrder());
        return aliases;
    }

    /** MULTI_ABI_ALIAS_COMPARATOR: lexicographic over the sorted aliases. */
    private static int compareMultiAbi(List<Integer> a, List<Integer> b) {
        for (int i = 0; i < Math.min(a.size(), b.size()); i++) {
            int c = Integer.compare(a.get(i), b.get(i));
            if (c != 0) return c;
        }
        return Integer.compare(a.size(), b.size());
    }

    // --- ScreenDensityMatcher ------------------------------------------------------------

    private static int dpi(Msg density) {
        if (density.has(2)) return (int) density.varint(2);
        int alias = (int) density.varint(1);
        return alias >= 0 && alias < DENSITY_DPI.length ? DENSITY_DPI[alias] : DEFAULT_DENSITY;
    }

    private static boolean densityMatches(Msg t, PackageBundles.Device device) {
        List<Integer> all = new ArrayList<>();
        for (Msg value : t.messages(VALUE)) all.add(dpi(value));
        int values = all.size();
        for (Msg value : t.messages(ALTERNATIVES)) all.add(dpi(value));
        if (all.isEmpty()) return true;
        int best = all.get(0);
        for (int dpi : all) if (compareDensity(dpi, best, device.densityDpi) > 0) best = dpi;
        return all.subList(0, values).contains(best);
    }

    /**
     * ScreenDensitySelector's comparator, Android's resource density choice
     * (ResTable_config::isBetterThan): positive when {@code a} matches {@code desired} better.
     */
    static int compareDensity(int a, int b, int desired) {
        if (desired == DEFAULT_DENSITY || desired == ANY_DENSITY) desired = MDPI;
        if (a == b) return 0;
        if (a == ANY_DENSITY) return 1;
        if (b == ANY_DENSITY) return -1;
        if (a > b) return -compareOrdered(b, a, desired);
        return compareOrdered(a, b, desired);
    }

    /** 1 if {@code lower} is better, -1 if {@code higher} is. */
    private static int compareOrdered(int lower, int higher, int desired) {
        if (desired >= higher) return -1;
        if (desired <= lower) return 1;
        // Scaling down is twice as good as scaling up (ResourceTypes.cpp).
        return ((2 * lower) - desired) * higher > desired * desired ? 1 : -1;
    }

    // --- LanguageMatcher, TextureCompressionFormatMatcher, CountrySetApkMatcher,
    //     DeviceTierApkMatcher, DeviceGroupApkMatcher, DeviceGroupModuleMatcher ------------

    private static boolean languageMatches(Msg t, PackageBundles.Device device) {
        if (t.isEmpty()) return true;
        List<String> values = t.strings(VALUE);
        if (!values.isEmpty()) {
            for (String value : values) if (device.languages.contains(value)) return true;
            return false;
        }
        // The fallback split: unless its alternatives cover every language of the device.
        return !new HashSet<>(t.strings(ALTERNATIVES)).containsAll(device.languages);
    }

    /** Gesher's device lists no texture compression formats: only untargeted or fallback APKs. */
    private static boolean tcfMatches(Msg t) {
        return t.isEmpty() || (t.messages(VALUE).isEmpty() && !t.messages(ALTERNATIVES).isEmpty());
    }

    /**
     * The set's default country set; without one, only untargeted or fallback APKs, as
     * Gesher's device names no country set of its own.
     */
    private static boolean countrySetMatches(Msg t, Spec spec) {
        if (t.isEmpty()) return true;
        if (spec.countrySet.isEmpty()) return t.strings(VALUE).isEmpty() && !t.strings(ALTERNATIVES).isEmpty();
        return t.strings(VALUE).contains(spec.countrySet);
    }

    private static void checkCountrySetCompatible(Msg t, Spec spec) throws IOException {
        if (t.isEmpty() || spec.countrySet.isEmpty()) return;
        if (t.strings(VALUE).contains(spec.countrySet) || t.strings(ALTERNATIVES).contains(spec.countrySet)) return;
        throw new IOException("The APK set's default country set " + spec.countrySet
                + " is none of those its splits target");
    }

    /** The set's default device tier (0 without one): only the APKs for that tier. */
    private static boolean tierMatches(Msg t, Spec spec) {
        if (t.isEmpty()) return true;
        for (Msg value : t.messages(TIER_VALUE)) if ((int) value.varint(1) == spec.tier) return true;
        return false;
    }

    private static void checkTierCompatible(Msg t, Spec spec) throws IOException {
        if (t.isEmpty()) return;
        List<Msg> all = new ArrayList<>(t.messages(TIER_VALUE));
        all.addAll(t.messages(TIER_ALTERNATIVES));
        for (Msg value : all) if ((int) value.varint(1) == spec.tier) return;
        throw new IOException("The APK set's device tier " + spec.tier + " is none of those its splits target");
    }

    /**
     * An APK or conditional module targeted at device groups matches only when it names the
     * set's default group (the empty group without one): Gesher's device is in no group.
     */
    private static boolean groupMatches(Msg t, Spec spec) {
        return t.isEmpty() || t.strings(VALUE).contains(spec.group);
    }

    /** bundletool's ResourcesUtils.convertLocaleToLanguage: the language, in its legacy code. */
    static String language(String locale) {
        String language = Locale.forLanguageTag(locale.replace('_', '-')).getLanguage();
        switch (language) {
            case "he": return "iw";
            case "id": return "in";
            case "yi": return "ji";
            default: return language;
        }
    }

    // --- protobuf wire format ------------------------------------------------------------

    /**
     * A message read in place from the toc's bytes: each field's number and its varint (or
     * fixed) value or, length-delimited, its offset and length. A nested message is read
     * once, when first asked for, and every field read counts against one budget for the
     * whole toc ({@link #MAX_DECODED_FIELDS}).
     */
    private static final class Msg {
        static final Msg EMPTY = new Msg(new byte[0], new int[1]);
        private final byte[] bytes;
        private final int[] budget;
        private int count;
        /** Field number << 1, | 1 when length-delimited. */
        private int[] keys = new int[0];
        /** The varint or fixed value, or offset << 32 | length. */
        private long[] values = new long[0];
        private Msg[] decoded;

        private Msg(byte[] bytes, int[] budget) {
            this.bytes = bytes;
            this.budget = budget;
        }

        static Msg parse(byte[] bytes, int[] budget) throws IOException {
            return parse(bytes, 0, bytes.length, budget);
        }

        private static Msg parse(byte[] bytes, int start, int end, int[] budget) throws IOException {
            Msg msg = new Msg(bytes, budget);
            int[] at = {start};
            while (at[0] < end) {
                if (--budget[0] < 0) {
                    throw new IOException("toc.pb has more than " + MAX_DECODED_FIELDS + " fields");
                }
                long key = varint(bytes, at, end);
                long field = key >>> 3;
                if (field < 1 || field > Integer.MAX_VALUE >> 1) throw new IOException("Invalid toc.pb");
                long value;
                boolean delimited = false;
                switch ((int) (key & 7)) {
                    case 0: value = varint(bytes, at, end); break;
                    case 1: value = fixed(bytes, at, end, 8); break;
                    case 2: {
                        long length = varint(bytes, at, end);
                        if (length < 0 || length > end - at[0]) throw new IOException("Invalid toc.pb");
                        value = ((long) at[0] << 32) | length;
                        at[0] += (int) length;
                        delimited = true;
                        break;
                    }
                    case 5: value = fixed(bytes, at, end, 4); break;
                    default: throw new IOException("Invalid toc.pb");
                }
                msg.add(((int) field << 1) | (delimited ? 1 : 0), value);
            }
            return msg;
        }

        private void add(int key, long value) {
            if (count == keys.length) {
                int capacity = Math.max(2, count * 2);
                keys = java.util.Arrays.copyOf(keys, capacity);
                values = java.util.Arrays.copyOf(values, capacity);
            }
            keys[count] = key;
            values[count++] = value;
        }

        private static long varint(byte[] bytes, int[] at, int end) throws IOException {
            long value = 0;
            for (int shift = 0; shift < 64; shift += 7) {
                if (at[0] >= end) throw new IOException("Invalid toc.pb");
                byte b = bytes[at[0]++];
                value |= (long) (b & 0x7f) << shift;
                if (b >= 0) return value;
            }
            throw new IOException("Invalid toc.pb");
        }

        private static long fixed(byte[] bytes, int[] at, int end, int size) throws IOException {
            if (end - at[0] < size) throw new IOException("Invalid toc.pb");
            long value = 0;
            for (int i = size - 1; i >= 0; i--) value = (value << 8) | (bytes[at[0] + i] & 0xff);
            at[0] += size;
            return value;
        }

        boolean isEmpty() { return count == 0; }

        boolean has(int field) {
            for (int i = 0; i < count; i++) if (keys[i] >>> 1 == field) return true;
            return false;
        }

        List<Msg> messages(int field) {
            List<Msg> list = new ArrayList<>();
            for (int i = 0; i < count; i++) {
                if (keys[i] >>> 1 != field) continue;
                if ((keys[i] & 1) == 0) {
                    list.add(EMPTY);
                    continue;
                }
                if (decoded == null) decoded = new Msg[count];
                if (decoded[i] == null) {
                    int offset = (int) (values[i] >>> 32);
                    try {
                        decoded[i] = parse(bytes, offset, offset + (int) values[i], budget);
                    } catch (IOException e) {
                        throw new UncheckedIOException(e);
                    }
                }
                list.add(decoded[i]);
            }
            return list;
        }

        Msg message(int field) {
            List<Msg> list = messages(field);
            return list.isEmpty() ? EMPTY : list.get(list.size() - 1);
        }

        long varint(int field) {
            for (int i = count - 1; i >= 0; i--) {
                if (keys[i] >>> 1 == field) return (keys[i] & 1) == 0 ? values[i] : 0;
            }
            return 0;
        }

        List<String> strings(int field) {
            List<String> list = new ArrayList<>();
            for (int i = 0; i < count; i++) {
                if (keys[i] != ((field << 1) | 1)) continue;
                list.add(new String(bytes, (int) (values[i] >>> 32), (int) values[i], StandardCharsets.UTF_8));
            }
            return list;
        }

        String string(int field) {
            List<String> list = strings(field);
            return list.isEmpty() ? "" : list.get(list.size() - 1);
        }
    }
}
