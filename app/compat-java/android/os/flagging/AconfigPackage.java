/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.os.flagging;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.Locale;

/**
 * The ConfigInfrastructure module's reader of aconfig flag values, which framework.jar
 * compiles against but does not contain. On a device it reads the flag storage files
 * (/metadata/aconfig); Gesher has no such storage: the flag values Gesher runs with are the
 * ones its framework was built with, compiled into each flag package's generated
 * {@code <package>.Flags} class (tools/gen/aconfig_java.py). This answers from those.
 *
 * <p>A package with no such class on the boot class path is not on the device:
 * {@link #load} throws {@link AconfigStorageReadException#ERROR_PACKAGE_NOT_FOUND}, as the
 * module does for a package its storage lacks. A flag the package does not declare answers
 * the caller's default, as on a device.
 *
 * <p>Framework's manifest parser (com.android.internal.pm.pkg.component.AconfigFlags) reads
 * the {@code android:featureFlag} of manifest elements through this class: without it,
 * parsing a manifest with one (framework-res.apk's has many) died on a NoClassDefFoundError.
 */
public class AconfigPackage {
    private final String mPackageName;
    private final Class<?> mFlags;

    private AconfigPackage(String packageName, Class<?> flags) {
        mPackageName = packageName;
        mFlags = flags;
    }

    /**
     * The flag package {@code packageName}; throws {@link AconfigStorageReadException}
     * ({@code ERROR_PACKAGE_NOT_FOUND}) when Gesher has no such package.
     */
    public static AconfigPackage load(String packageName) {
        Class<?> flags = null;
        if (packageName != null && !packageName.isEmpty()) {
            try {
                Class<?> c = Class.forName(packageName + ".Flags", false,
                        AconfigPackage.class.getClassLoader());
                if (c.getName().equals(packageName + ".Flags")) flags = c;
            } catch (ClassNotFoundException | LinkageError e) {
                // Not a flag package Gesher has.
            }
        }
        if (flags == null) {
            throw new AconfigStorageReadException(
                    AconfigStorageReadException.ERROR_PACKAGE_NOT_FOUND,
                    "package " + packageName + " cannot be found on the device");
        }
        return new AconfigPackage(packageName, flags);
    }

    /** The value of the package's flag {@code flagName}, or {@code defaultValue} if none. */
    public boolean getBooleanFlagValue(String flagName, boolean defaultValue) {
        if (flagName == null || flagName.isEmpty()) return defaultValue;
        try {
            // A generated Flags class: FLAG_<NAME> names the flag, <camelName>() answers it.
            Field name = mFlags.getField("FLAG_" + flagName.toUpperCase(Locale.ROOT));
            if (!Modifier.isStatic(name.getModifiers())
                    || !(mPackageName + "." + flagName).equals(name.get(null))) {
                return defaultValue;
            }
            Method value = mFlags.getMethod(camel(flagName));
            if (!Modifier.isStatic(value.getModifiers()) || value.getReturnType() != boolean.class) {
                return defaultValue;
            }
            return (Boolean) value.invoke(null);
        } catch (ReflectiveOperationException | RuntimeException | LinkageError e) {
            return defaultValue;
        }
    }

    /** @hide The module's internal read, the same values here. */
    public boolean getBooleanFlagValueInternal(String flagName, boolean defaultValue) {
        return getBooleanFlagValue(flagName, defaultValue);
    }

    /** aconfig's accessor name: foo_bar_baz is fooBarBaz. */
    private static String camel(String name) {
        String[] words = name.split("_", -1);
        StringBuilder out = new StringBuilder(words[0]);
        for (int i = 1; i < words.length; i++) {
            if (!words[i].isEmpty()) {
                out.append(Character.toUpperCase(words[i].charAt(0))).append(words[i], 1,
                        words[i].length());
            }
        }
        return out.toString();
    }
}
