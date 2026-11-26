/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.content.pm.ApplicationInfo;
import android.content.pm.SharedLibraryInfo;
import android.content.res.AssetManager;

import com.gesher.server.SharedLibraryGraph;
import com.gesher.server.SharedLibraryRegistry;

import dalvik.system.PathClassLoader;

import java.io.File;
import java.lang.reflect.InvocationTargetException;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * Android's android.test.base, android.test.mock and android.test.runner shared libraries
 * (frameworks/base/test-base, test-mock and test-runner, with JUnit 3 from test-junit).
 * GloryFit names android.test.runner in <uses-library>, and its install failed with
 * INSTALL_FAILED_MISSING_SHARED_LIBRARY. The declarations Gesher ships
 * (tools/package/etc/permissions, platform.xml's) resolve through
 * com.gesher.server.SharedLibraryRegistry to the jars under the system root's framework
 * directory, android.test.runner with its declared dependencies; the libraries
 * PackageBackwardCompatibility adds are added (AndroidTestRunnerSplitUpdater,
 * AndroidTestBaseUpdater); the classes are real and load in the app's loader, not from
 * the boot class path.
 * args[0]: the system root ($ANDROID_ROOT). Prints "AndroidTestLibrariesTest all passed".
 */
public final class AndroidTestLibrariesTest {
    private static final String BASE = SharedLibraryRegistry.ANDROID_TEST_BASE;
    private static final String MOCK = SharedLibraryRegistry.ANDROID_TEST_MOCK;
    private static final String RUNNER = SharedLibraryRegistry.ANDROID_TEST_RUNNER;

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static File jar(File root, String library) throws Exception {
        return new File(root, "framework/" + library + ".jar").getCanonicalFile();
    }

    public static void main(String[] args) throws Exception {
        // android.util.Xml reads a system property, and the graph the system resources.
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        File root = new File(args[0]);
        SharedLibraryRegistry registry = SharedLibraryRegistry.read(root);
        List<String> none = Collections.emptyList();
        List<String> runner = Collections.singletonList(RUNNER);

        // Named as GloryFit names it: required, alone.
        SharedLibraryGraph graph = registry.resolve(runner, none, null);
        ApplicationInfo app = new ApplicationInfo();
        graph.apply(app);
        require(app.sharedLibraryInfos != null && app.sharedLibraryInfos.size() == 1,
                "the app's shared libraries are not android.test.runner alone");
        SharedLibraryInfo top = app.sharedLibraryInfos.get(0);
        require(RUNNER.equals(top.getName()) && top.getDependencies().size() == 2
                        && BASE.equals(top.getDependencies().get(0).getName())
                        && MOCK.equals(top.getDependencies().get(1).getName()),
                "android.test.runner's declared dependencies are not base then mock: " + top);
        require(app.sharedLibraryFiles != null && app.sharedLibraryFiles.length == 3,
                "the app's shared library files are not the three jars: "
                        + Arrays.toString(app.sharedLibraryFiles));
        for (String library : new String[] {RUNNER, BASE, MOCK}) {
            boolean found = false;
            for (String path : app.sharedLibraryFiles) {
                found |= path.endsWith("/framework/" + library + ".jar");
            }
            require(found, library + " is not the system root's framework jar: "
                    + Arrays.toString(app.sharedLibraryFiles));
        }
        System.out.println("AndroidTestLibrariesTest: android.test.runner resolves to "
                + "$ANDROID_ROOT/framework jars with android.test.base and android.test.mock");

        // PackageBackwardCompatibility, in its order.
        require(Arrays.asList(BASE, MOCK, RUNNER).equals(registry.withImplicit(29, runner, none).required),
                "API 29 with android.test.runner is not base, mock, runner");
        require(Arrays.asList(BASE, MOCK, RUNNER).equals(registry.withImplicit(36, runner, none).required),
                "API 36 with android.test.runner is not base, mock, runner (implicit dependencies)");
        SharedLibraryRegistry.Uses optional = registry.withImplicit(36, none, runner);
        require(optional.required.isEmpty()
                        && Arrays.asList(BASE, MOCK, RUNNER).equals(optional.optional),
                "an optional android.test.runner does not get optional dependencies: "
                        + optional.required + " " + optional.optional);
        require(Collections.singletonList(BASE).equals(registry.withImplicit(29, none, none).required),
                "an app targeting API 29 does not get android.test.base");
        require(registry.withImplicit(30, none, none).required.isEmpty(),
                "an app targeting API 30 gets android.test.base without naming it");
        require(Collections.singletonList(BASE).equals(registry.withImplicit(29, none,
                        Collections.singletonList(BASE)).optional)
                        && registry.withImplicit(29, none, Collections.singletonList(BASE))
                        .required.isEmpty(),
                "an app naming android.test.base optional gets it required as well");
        System.out.println("AndroidTestLibrariesTest: apps targeting API 29 or lower get "
                + "android.test.base, android.test.runner brings mock and base");

        // An absent optional library contributes nothing; an absent required one fails.
        require(registry.resolve(none, Collections.singletonList("gesher.absent"), null).isEmpty(),
                "an absent optional library contributed a loader");
        try {
            registry.resolve(Collections.singletonList("gesher.absent"), none, null);
            throw new AssertionError("an absent required library resolved");
        } catch (java.io.IOException expected) {
            require(expected.getMessage().contains("INSTALL_FAILED_MISSING_SHARED_LIBRARY"),
                    "an absent required library failed with " + expected.getMessage());
        }

        // Not on the boot class path; real classes in the app's loader over the jars.
        for (String name : new String[] {"junit.framework.TestCase", "android.test.mock.MockContext",
                "android.test.InstrumentationTestRunner"}) {
            try {
                Class.forName(name, false, null);
                throw new AssertionError(name + " is on the boot class path");
            } catch (ClassNotFoundException expected) {
            }
        }
        ClassLoader loader = new PathClassLoader(jar(root, BASE) + File.pathSeparator
                + jar(root, MOCK) + File.pathSeparator + jar(root, RUNNER),
                ClassLoader.getSystemClassLoader().getParent());
        Class<?> assertClass = loader.loadClass("junit.framework.Assert");
        assertClass.getMethod("assertEquals", int.class, int.class).invoke(null, 3, 3);
        try {
            assertClass.getMethod("assertEquals", int.class, int.class).invoke(null, 3, 4);
            throw new AssertionError("junit.framework.Assert accepted 3 == 4");
        } catch (InvocationTargetException expected) {
            require(expected.getCause().getClass().getName()
                            .equals("junit.framework.AssertionFailedError"),
                    "Assert failed with " + expected.getCause());
        }
        Object mock = loader.loadClass("android.test.mock.MockContext").getConstructor().newInstance();
        try {
            mock.getClass().getMethod("getPackageName").invoke(mock);
            throw new AssertionError("MockContext.getPackageName did not throw");
        } catch (InvocationTargetException expected) {
            require(expected.getCause() instanceof UnsupportedOperationException,
                    "MockContext.getPackageName threw " + expected.getCause());
        }
        Class<?> instrumentation = loader.loadClass("android.test.InstrumentationTestRunner");
        require(android.app.Instrumentation.class.isAssignableFrom(instrumentation)
                        && instrumentation.getClassLoader() == loader,
                "InstrumentationTestRunner is not the jar's Instrumentation");
        Object suite = loader.loadClass("junit.framework.TestSuite").getConstructor().newInstance();
        Object result = loader.loadClass("junit.textui.TestRunner").getMethod("run",
                loader.loadClass("junit.framework.Test")).invoke(null, suite);
        require((Integer) result.getClass().getMethod("runCount").invoke(result) == 0
                        && (Boolean) result.getClass().getMethod("wasSuccessful").invoke(result),
                "junit.textui.TestRunner did not run an empty suite");
        System.out.println("AndroidTestLibrariesTest: junit.framework, android.test.mock and "
                + "android.test.InstrumentationTestRunner load from the jars, not the boot class path");
        System.out.println("AndroidTestLibrariesTest all passed");
    }
}
