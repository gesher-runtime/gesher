/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;

import java.io.IOException;
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
 * (github.com/google/bundletool, {@code com.android.tools.build.bundletool.device}).
 *
 * <p>The protobuf wire format is read directly: the fields below are those of bundletool's
 * {@code commands.proto} and {@code targeting.proto}.
 */
final class ApkSetToc {
    private ApkSetToc() {}

    // BuildApksResult
    private static final int RESULT_VARIANT = 1, RESULT_ASSET_SLICE_SET = 3;
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
    private static final int VALUE = 1, ALTERNATIVES = 2;
    private static final int INSTALL_TIME = 1;  // DeliveryType.INSTALL_TIME

    /** Abi.AbiAlias in enum order, which is also bundletool's ARCHITECTURE_ORDERING. */
    private static final String[] ABI_ALIASES = {null, "armeabi", "armeabi-v7a", "arm64-v8a", "x86",
            "x86_64", "mips", "mips64", "riscv64"};
    /** ScreenDensity.DensityAlias to dpi (bundletool's ResourcesUtils.DENSITY_ALIAS_TO_DPI_MAP). */
    private static final int[] DENSITY_DPI = {0, 0xffff, 120, 160, 213, 240, 320, 480, 640};
    private static final int DEFAULT_DENSITY = 0, ANY_DENSITY = 0xfffe, MDPI = 160;

    /** The paths, inside the APK set, of the APKs {@code device} gets. */
    static List<String> select(byte[] toc, PackageBundles.Device device) throws IOException {
        Msg result = Msg.parse(toc);
        List<String> paths = new ArrayList<>();
        Msg variant = variant(result.messages(RESULT_VARIANT), device);
        Set<String> modules = installTimeModules(variant, device);
        for (Msg set : variant.messages(VARIANT_APK_SET)) {
            if (!modules.contains(set.message(SET_MODULE).string(MODULE_NAME))) continue;
            for (Msg apk : set.messages(SET_APK)) {
                checkCompatible(apk.message(APK_TARGETING), device);
                if (matches(apk.message(APK_TARGETING), device)) paths.add(apk.string(APK_PATH));
            }
        }
        for (Msg slices : result.messages(RESULT_ASSET_SLICE_SET)) {
            // Asset packs: install-time ones only (ApkMatcher.getUpfrontAssetModules).
            if (slices.message(SLICES_MODULE).varint(ASSET_DELIVERY_TYPE) != INSTALL_TIME) continue;
            for (Msg apk : slices.messages(SLICES_APK)) {
                if (matches(apk.message(APK_TARGETING), device)) paths.add(apk.string(APK_PATH));
            }
        }
        return paths;
    }

    /**
     * The one variant that matches (VariantMatcher.getMatchingVariant): not an instant one,
     * not one that requires the SDK runtime (Gesher has none), and matching every dimension
     * the device defines.
     */
    private static Msg variant(List<Msg> variants, PackageBundles.Device device) throws IOException {
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

    private static boolean isInstant(Msg variant) {
        boolean any = false;
        for (Msg set : variant.messages(VARIANT_APK_SET)) {
            for (Msg apk : set.messages(SET_APK)) {
                if (!apk.has(APK_INSTANT)) return false;
                any = true;
            }
        }
        return any;
    }

    /**
     * The install-time modules whose conditions the device meets, with every module they
     * depend on ({@code ApkMatcher.getInstallTimeAndRequestedModulesWithDependencies}; every
     * module depends on base). A set from bundletool before 0.10.2 has no delivery type and
     * marks on-demand modules with the deprecated flag instead. Device features and OpenGL
     * versions are conditions Gesher's device spec does not list, so, as bundletool does for
     * a spec without them, they do not exclude a module.
     */
    private static Set<String> installTimeModules(Msg variant, PackageBundles.Device device) {
        Map<String, List<String>> dependencies = new HashMap<>();
        Set<String> modules = new LinkedHashSet<>();
        for (Msg set : variant.messages(VARIANT_APK_SET)) {
            Msg module = set.message(SET_MODULE);
            String name = module.string(MODULE_NAME);
            List<String> needs = new ArrayList<>(module.strings(MODULE_DEPENDENCIES));
            needs.add("base");
            dependencies.put(name, needs);
            long delivery = module.varint(MODULE_DELIVERY_TYPE);
            boolean installTime = delivery != 0 ? delivery == INSTALL_TIME
                    : module.varint(MODULE_ON_DEMAND_DEPRECATED) == 0;
            Msg targeting = module.message(MODULE_TARGETING);
            if (installTime && sdkMatches(targeting.message(MT_SDK), device)
                    && targeting.message(MT_DEVICE_GROUP).strings(VALUE).isEmpty()) {
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
    private static boolean matches(Msg t, PackageBundles.Device device) throws IOException {
        if (!t.message(AT_DEVICE_TIER).isEmpty()) {
            // DeviceTierApkMatcher requires the spec to name a tier; Gesher's device has none.
            throw new IOException("The APK set targets device tiers, which this device does not define");
        }
        return sdkMatches(t.message(AT_SDK), device)
                && abiMatches(t.message(AT_ABI), device)
                && multiAbiMatches(t.message(AT_MULTI_ABI), device)
                && (device.densityDpi == 0 || densityMatches(t.message(AT_DENSITY), device))
                && (device.languages == null || languageMatches(t.message(AT_LANGUAGE), device))
                && tcfMatches(t.message(AT_TCF))
                && t.message(AT_DEVICE_GROUP).strings(VALUE).isEmpty()
                && countrySetMatches(t.message(AT_COUNTRY_SET));
    }

    private static void checkCompatible(Msg t, PackageBundles.Device device) throws IOException {
        checkSdkCompatible(t.message(AT_SDK), device);
        checkAbiCompatible(t.message(AT_ABI), device);
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

    // --- LanguageMatcher, TextureCompressionFormatMatcher, CountrySetApkMatcher ----------

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

    /** Gesher's device names no country set: only untargeted or fallback APKs. */
    private static boolean countrySetMatches(Msg t) {
        return t.isEmpty() || (t.strings(VALUE).isEmpty() && !t.strings(ALTERNATIVES).isEmpty());
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

    /** A decoded message: each field's values, varints as Long, length-delimited as byte[]. */
    private static final class Msg {
        static final Msg EMPTY = new Msg();
        private final Map<Integer, List<Object>> fields = new HashMap<>();

        static Msg parse(byte[] bytes) throws IOException {
            Msg msg = new Msg();
            int[] at = {0};
            while (at[0] < bytes.length) {
                long key = varint(bytes, at);
                int field = (int) (key >>> 3);
                Object value;
                switch ((int) (key & 7)) {
                    case 0: value = varint(bytes, at); break;
                    case 1: value = fixed(bytes, at, 8); break;
                    case 2: {
                        long length = varint(bytes, at);
                        if (length < 0 || length > bytes.length - at[0]) throw new IOException("Invalid toc.pb");
                        byte[] data = new byte[(int) length];
                        System.arraycopy(bytes, at[0], data, 0, data.length);
                        at[0] += data.length;
                        value = data;
                        break;
                    }
                    case 5: value = fixed(bytes, at, 4); break;
                    default: throw new IOException("Invalid toc.pb");
                }
                if (field < 1) throw new IOException("Invalid toc.pb");
                msg.fields.computeIfAbsent(field, f -> new ArrayList<>()).add(value);
            }
            return msg;
        }

        private static long varint(byte[] bytes, int[] at) throws IOException {
            long value = 0;
            for (int shift = 0; shift < 64; shift += 7) {
                if (at[0] >= bytes.length) throw new IOException("Invalid toc.pb");
                byte b = bytes[at[0]++];
                value |= (long) (b & 0x7f) << shift;
                if (b >= 0) return value;
            }
            throw new IOException("Invalid toc.pb");
        }

        private static long fixed(byte[] bytes, int[] at, int size) throws IOException {
            if (bytes.length - at[0] < size) throw new IOException("Invalid toc.pb");
            long value = 0;
            for (int i = size - 1; i >= 0; i--) value = (value << 8) | (bytes[at[0] + i] & 0xff);
            at[0] += size;
            return value;
        }

        boolean isEmpty() { return fields.isEmpty(); }
        boolean has(int field) { return fields.containsKey(field); }

        List<Msg> messages(int field) {
            List<Msg> list = new ArrayList<>();
            for (Object value : fields.getOrDefault(field, Collections.emptyList())) {
                try {
                    list.add(value instanceof byte[] ? parse((byte[]) value) : EMPTY);
                } catch (IOException e) {
                    throw new IllegalArgumentException("Invalid toc.pb", e);
                }
            }
            return list;
        }

        Msg message(int field) {
            List<Msg> list = messages(field);
            return list.isEmpty() ? EMPTY : list.get(list.size() - 1);
        }

        long varint(int field) {
            List<Object> values = fields.get(field);
            Object last = values == null ? null : values.get(values.size() - 1);
            return last instanceof Long ? (Long) last : 0;
        }

        List<String> strings(int field) {
            List<String> list = new ArrayList<>();
            for (Object value : fields.getOrDefault(field, Collections.emptyList())) {
                if (value instanceof byte[]) list.add(new String((byte[]) value, StandardCharsets.UTF_8));
            }
            return list;
        }

        String string(int field) {
            List<String> list = strings(field);
            return list.isEmpty() ? "" : list.get(list.size() - 1);
        }
    }
}
