/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.content.pm.ApplicationInfo;
import android.content.res.AssetManager;

import dalvik.system.PathClassLoader;

import java.io.File;
import java.lang.reflect.Method;
import java.util.Collections;
import java.util.List;

/**
 * The org.apache.http.legacy shared library (external/apache-http), which Gmail names in
 * <uses-library android:required="false">: its "Add email account" provider screens died
 * on NoClassDefFoundError for org.apache.http.params.BasicHttpParams. The declaration
 * Gesher ships (tools/package/etc/permissions, with Android's /system path) resolves
 * through com.gesher.server.SharedLibraryRegistry to the jar under the system root's
 * framework directory; apps targeting API 27 or lower get the library without naming it
 * (OrgApacheHttpLegacyUpdater); its classes load in the app's loader, not from the boot
 * class path.
 * args[0]: the system root ($ANDROID_ROOT). Prints "HttpLegacyTest all passed".
 */
public final class HttpLegacyTest {
    private static final String LIBRARY = "org.apache.http.legacy";

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    public static void main(String[] args) throws Exception {
        // android.util.Xml reads a system property, and the graph the system resources:
        // gesher-app's Main has both in an app.
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        File root = new File(args[0]);
        Class<?> registryClass = Class.forName("com.gesher.server.SharedLibraryRegistry");
        Object registry = registryClass.getMethod("read", File.class).invoke(null, root);
        Method resolve = registryClass.getMethod("resolve", List.class, List.class, String.class);
        Method withImplicit = registryClass.getMethod("withImplicit", int.class, List.class, List.class);
        List<String> none = Collections.emptyList();
        List<String> named = Collections.singletonList(LIBRARY);

        // Named as Gmail names it: optional.
        Object graph = resolve.invoke(registry, none, named, null);
        ApplicationInfo app = new ApplicationInfo();
        graph.getClass().getMethod("apply", ApplicationInfo.class).invoke(graph, app);
        require(app.sharedLibraryFiles != null && app.sharedLibraryFiles.length == 1,
                "the app's shared library files are not the one jar");
        File jar = new File(app.sharedLibraryFiles[0]);
        require(jar.getCanonicalFile().equals(
                new File(root, "framework/org.apache.http.legacy.jar").getCanonicalFile()),
                "the library resolved to " + jar + ", not the system root's framework jar");
        System.out.println("HttpLegacyTest: org.apache.http.legacy resolves to "
                + "$ANDROID_ROOT/framework/org.apache.http.legacy.jar");

        // OrgApacheHttpLegacyUpdater: first for API 27 or lower, not for 28; not twice.
        require(named.equals(withImplicit.invoke(registry, 27, none, none)),
                "an app targeting API 27 does not get the library");
        require(((List<?>) withImplicit.invoke(registry, 28, none, none)).isEmpty(),
                "an app targeting API 28 gets the library");
        require(((List<?>) withImplicit.invoke(registry, 27, none, named)).isEmpty(),
                "an app naming the library optional gets it required as well");
        System.out.println("HttpLegacyTest: apps targeting API 27 or lower get it implicitly, "
                + "API 28 and up only by name");

        // Not on the boot class path; in the app's loader over the jar.
        try {
            Class.forName("org.apache.http.params.BasicHttpParams", false, null);
            throw new AssertionError("BasicHttpParams is on the boot class path");
        } catch (ClassNotFoundException expected) {
        }
        ClassLoader loader = new PathClassLoader(jar.getPath(),
                ClassLoader.getSystemClassLoader().getParent());
        Class<?> paramsClass = loader.loadClass("org.apache.http.params.HttpParams");
        Object params = loader.loadClass("org.apache.http.params.BasicHttpParams")
                .getConstructor().newInstance();
        Class<?> connection = loader.loadClass("org.apache.http.params.HttpConnectionParams");
        connection.getMethod("setConnectionTimeout", paramsClass, int.class).invoke(null, params, 15000);
        int timeout = (Integer) connection.getMethod("getConnectionTimeout", paramsClass)
                .invoke(null, params);
        require(timeout == 15000, "HttpConnectionParams gave a timeout of " + timeout);
        Object get = loader.loadClass("org.apache.http.client.methods.HttpGet")
                .getConstructor(String.class).newInstance("https://example.org/path?q=1");
        Object uri = get.getClass().getMethod("getURI").invoke(get);
        require("example.org".equals(uri.getClass().getMethod("getHost").invoke(uri))
                        && "GET".equals(get.getClass().getMethod("getMethod").invoke(get)),
                "HttpGet's request line is wrong");
        System.out.println("HttpLegacyTest: BasicHttpParams, HttpConnectionParams and HttpGet "
                + "load from the jar, not the boot class path");
        System.out.println("HttpLegacyTest all passed");
    }
}
