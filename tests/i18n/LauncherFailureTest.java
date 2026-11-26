/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.app;

import com.gesher.app.locale.UiMessages;

/** Host tests for the production Launcher.describeFailure body extracted by check.py. */
public final class LauncherFailureTest {
    private static final String CODE = "INSTALL_FAILED_NO_MATCHING_ABIS";
    private static final String SUFFIX = "; Gesher runs x86_64 and arm64-v8a"
            + " (use the app's arm64-v8a, x86_64 or universal APK)";
    private static String language;
    private static int checks;

    static String text(String key, Object... args) {
        return UiMessages.text(key, language, args);
    }

    private static void equal(String expected, String actual) {
        if (expected == null ? actual != null : !expected.equals(actual)) {
            throw new AssertionError(language + ": " + expected + " != " + actual);
        }
        checks++;
    }

    private static String diagnostic(String name, String abis) {
        return name + " has native code only for " + abis + SUFFIX;
    }

    private static String describe(String detail) {
        return Launcher.describeFailure(new IllegalArgumentException(CODE + ": " + detail));
    }

    private static void structured(String name, String abis) {
        equal(text("install_no_matching_abis", name, abis), describe(diagnostic(name, abis)));
    }

    private static void generic(String detail) {
        equal(text("install_no_matching_abis_details", detail.trim().isEmpty() ? CODE : detail.trim()),
                describe(detail));
    }

    public static void main(String[] args) {
        for (String locale : new String[] {"en", "he"}) {
            language = locale;
            structured("מוביט.apk", "armeabi-v7a");
            structured("app (v2); {1}.apk", "armeabi-v7a, x86");
            structured("app.apk", "riscv64");
            structured("app.apk", "mips64");
            equal(text("install_no_matching_abis", "מוביט.apk", "armeabi-v7a"),
                    Launcher.describeFailure(new RuntimeException("remote failure",
                            new IllegalArgumentException("remote: " + CODE + ": "
                                    + diagnostic("מוביט.apk", "armeabi-v7a")))));

            // Filenames and arbitrary ZIP directory names can include delimiters.
            // Retain the original diagnostic rather than mislabelling its pieces.
            generic(diagnostic("app has native code only for test.apk", "mips64"));
            generic(diagnostic("app.apk", "odd;abi"));
            generic(diagnostic("app.apk", "odd has native code only for mips64"));
            generic(diagnostic("app.apk", "odd; Gesher runs mips64"));
            generic(diagnostic("app.apk", "ספריה"));
            generic(diagnostic("", "armeabi-v7a"));
            generic(diagnostic("app.apk", ""));
            generic("app.apk has native code only for armeabi-v7a");
            generic("no match for riscv64");
            generic("unknown {0} {1} detail");
            generic("");
            equal(text("install_no_matching_abis_details", CODE),
                    Launcher.describeFailure(new IllegalArgumentException(CODE)));

            // The fallback is localized and does not invent a 32-bit ABI diagnosis.
            String unknown = describe("no match for riscv64");
            if (unknown.contains("32-bit") || !unknown.contains("no match for riscv64")) {
                throw new AssertionError("Changed or invented the unsupported ABI: " + unknown);
            }
            checks++;
            equal(text("install_bad_signature", "com.example"), Launcher.describeFailure(
                    new SecurityException("INSTALL_FAILED_BAD_SIGNATURE: com.example")));
            equal("INSTALL_FAILED_OTHER: detail", Launcher.describeFailure(
                    new IllegalArgumentException("INSTALL_FAILED_OTHER: detail")));
            equal(null, Launcher.describeFailure(new RuntimeException("unrelated failure")));
            equal(null, Launcher.describeFailure(new RuntimeException((String) null)));
            String packageName = "com.example";
            for (String feature : new String[] {"android.hardware.location",
                    "android.hardware.location.gps", "android.hardware.location.network"}) {
                for (String key : new String[] {"edition_upgrade_required",
                        "edition_license_unavailable", "edition_trial_limit"}) {
                    equal(text(key, packageName, text("edition_feature_location")),
                            Launcher.describeEditionFailure(key, packageName + "\t" + feature));
                }
            }
            equal(null, Launcher.describeEditionFailure("edition_upgrade_required", null));
            equal(null, Launcher.describeEditionFailure("edition_upgrade_required",
                    "com.example\tandroid.hardware.unknown"));
            equal(null, Launcher.describeEditionFailure("edition_upgrade_required",
                    "com.example\tandroid.hardware.location\textra"));
            equal(null, Launcher.describeEditionFailure("edition_upgrade_required",
                    "com.\nexample\tandroid.hardware.location"));
            equal(null, Launcher.describeFailure(
                    new RuntimeException("6401: com.example\tandroid.hardware.location")));
        }
        System.out.println("PASS: Launcher failure mapping " + checks
                + " checks (real mapper and catalog; host language selection)");
    }
}
