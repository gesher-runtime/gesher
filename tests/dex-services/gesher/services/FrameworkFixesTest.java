/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.app.ActivityThread;
import android.app.Application;
import android.content.res.AssetManager;
import android.content.res.Resources;
import android.graphics.Typeface;
import android.text.Html;
import android.text.Spanned;
import android.text.style.StyleSpan;
import android.text.style.URLSpan;

import java.io.File;
import java.io.RandomAccessFile;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Executor;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.channels.OverlappingFileLockException;

/**
 * Small fixes on the boot class path, each of which failed in an app:
 *  - android.graphics.ColorMatrix (app/java, commit bf94f958): framework.jar does not
 *    have it, and an app's ColorMatrixColorFilter died with NoClassDefFoundError;
 *  - FileChannel.lock / tryLock / release (FileDispatcherImpl.lock0 / release0 over
 *    fcntl, compat/devsvc/devsvc_natives.cc, commits 93d60cff and d768300d): a blocking
 *    lock that hung or failed, as SharedPreferences' and Room's lock files take;
 *  - android.view.RenderNode and DisplayListCanvas (the pre-API-29 names, commit
 *    9ff23f33), which older UI libraries look up by reflection: present with the
 *    methods they call (not called here: android.graphics.RenderNode needs hwui);
 *  - android.net.nsd.NsdManager and NsdServiceInfo (app/java, the Connectivity module's
 *    API): an app store that names NsdManager.RegistrationListener died with
 *    NoClassDefFoundError; a discovery starts (and finds nothing), registering fails
 *    through the listener, and unregistering a failed registration does not throw;
 *  - com.gesher.app.NativeLibs over a base APK and its splits: an .xapk's native
 *    libraries are in an ABI split (config.arm64_v8a.apk), and the package was installed
 *    as x86_64 without them (Mercantile's cheque scanner: dlopen of a missing lib).
 *  - a stand-in service's unimplemented method that returns a list answers an empty
 *    one, not null (com.gesher.server.StandIn): ShortcutManager.getDynamicShortcuts
 *    called getList() on null (Wikipedia).
 *  - org.ccil.cowan.tagsoup (frameworks/base's ext boot jar, compile only in the framework
 *    build; its classes in gesher-app.jar, app/build.sh): android.text.Html.fromHtml died
 *    on a stackless NoClassDefFoundError ("Class not found using the boot class loader"),
 *    and Wikipedia's onboarding stopped on its first page (its next page's Compose text
 *    goes through Html.fromHtml; the error was captured in composition).
 * Prints "FrameworkFixesTest all passed".
 */
public final class FrameworkFixesTest {
    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static boolean near(float a, float b) {
        return Math.abs(a - b) < 1e-5f;
    }

    private static void colorMatrix() throws Exception {
        Class<?> type = Class.forName("android.graphics.ColorMatrix");
        Object m = type.getConstructor().newInstance();
        Method getArray = type.getMethod("getArray");
        float[] identity = (float[]) getArray.invoke(m);
        require(identity.length == 20 && identity[0] == 1 && identity[6] == 1 && identity[12] == 1
                && identity[18] == 1 && identity[4] == 0, "a new ColorMatrix is not the identity");
        type.getMethod("setSaturation", float.class).invoke(m, 0f);
        float[] gray = (float[]) getArray.invoke(m);
        for (int row = 0; row < 3; row++) {
            require(near(gray[row * 5], 0.213f) && near(gray[row * 5 + 1], 0.715f)
                    && near(gray[row * 5 + 2], 0.072f), "setSaturation(0) row " + row + " is wrong");
        }
        require(near(gray[18], 1f), "setSaturation(0) changed alpha");
        Object scale = type.getConstructor().newInstance();
        type.getMethod("setScale", float.class, float.class, float.class, float.class)
                .invoke(scale, 2f, 2f, 2f, 1f);
        type.getMethod("postConcat", type).invoke(m, scale);
        float[] scaled = (float[]) getArray.invoke(m);
        require(near(scaled[0], 0.426f) && near(scaled[6], 1.43f) && near(scaled[18], 1f),
                "postConcat(setScale(2)) is wrong");
        System.out.println("FrameworkFixesTest: ColorMatrix on the boot class path: identity, "
                + "setSaturation, setScale, postConcat");
    }

    private static void fileLocks() throws Exception {
        String data = System.getenv("ANDROID_DATA");
        File file = new File(data != null ? data : ".", "framework-fixes-lock.tmp");
        try (RandomAccessFile raf = new RandomAccessFile(file, "rw");
                FileChannel channel = raf.getChannel()) {
            raf.write(new byte[64]);
            FileLock whole = channel.lock();
            require(whole.isValid() && !whole.isShared(), "lock() is not an exclusive valid lock");
            boolean overlapped = false;
            try {
                channel.tryLock(0, 8, false);
            } catch (OverlappingFileLockException expected) {
                overlapped = true;
            }
            require(overlapped, "a second lock over the same range was granted");
            whole.release();
            require(!whole.isValid(), "release() left the lock valid");
            FileLock part = channel.tryLock(0, 16, true);
            require(part != null && part.isShared() && part.isValid(), "tryLock(shared) failed");
            FileLock other = channel.tryLock(32, 16, false);
            require(other != null && other.isValid(), "tryLock of a disjoint range failed");
            part.release();
            other.release();
            FileLock again = channel.lock(0, Long.MAX_VALUE, false);
            require(again.isValid(), "lock() after release failed");
            again.release();
        } finally {
            file.delete();
        }
        System.out.println("FrameworkFixesTest: FileChannel lock, tryLock (shared, ranges) and "
                + "release");
    }

    private static void legacyRenderNode() throws Exception {
        Class<?> view = Class.forName("android.view.View");
        Class<?> node = Class.forName("android.view.RenderNode");
        Class<?> canvas = Class.forName("android.view.DisplayListCanvas");
        Method create = node.getMethod("create", String.class, view);
        require(java.lang.reflect.Modifier.isStatic(create.getModifiers())
                && create.getReturnType() == node, "RenderNode.create(String, View) is not a factory");
        node.getMethod("setAlpha", float.class);
        node.getMethod("getAlpha");
        require(node.getMethod("start", int.class, int.class).getReturnType() == canvas,
                "RenderNode.start(int, int) does not return a DisplayListCanvas");
        node.getMethod("end", canvas);
        canvas.getMethod("drawRenderNode", node);
        require(Class.forName("android.graphics.Canvas").isAssignableFrom(canvas),
                "DisplayListCanvas is not a Canvas");
        System.out.println("FrameworkFixesTest: android.view.RenderNode and DisplayListCanvas "
                + "with create, start, end, drawRenderNode");
    }

    /** Object's methods of a listener proxy: by identity (NsdManager keys a map by it). */
    private static Object identity(Object proxy, Method method, Object[] a) {
        switch (method.getName()) {
            case "hashCode": return System.identityHashCode(proxy);
            case "equals": return proxy == a[0];
            default: return "listener@" + Integer.toHexString(System.identityHashCode(proxy));
        }
    }

    private static void nsd() throws Exception {
        Class<?> manager = Class.forName("android.net.nsd.NsdManager");
        Class<?> infoType = Class.forName("android.net.nsd.NsdServiceInfo");
        Class<?> discovery = Class.forName("android.net.nsd.NsdManager$DiscoveryListener");
        Class<?> registration = Class.forName("android.net.nsd.NsdManager$RegistrationListener");
        Class.forName("android.net.nsd.NsdManager$ResolveListener");
        Class.forName("android.net.nsd.NsdManager$ServiceInfoCallback");
        Class<?> looper = Class.forName("android.os.Looper");
        if (looper.getMethod("getMainLooper").invoke(null) == null) {
            looper.getMethod("prepareMainLooper").invoke(null);
        }
        Object nsd = manager.getConstructor(Class.forName("android.content.Context"))
                .newInstance((Object) null);
        Object info = infoType.getConstructor().newInstance();
        infoType.getMethod("setServiceName", String.class).invoke(info, "gesher-test");
        infoType.getMethod("setServiceType", String.class).invoke(info, "_gesher._tcp");
        infoType.getMethod("setPort", int.class).invoke(info, 4242);
        infoType.getMethod("setAttribute", String.class, String.class).invoke(info, "k", "v");

        List<String> calls = new ArrayList<>();
        Executor now = Runnable::run;
        Object onDiscovery = Proxy.newProxyInstance(discovery.getClassLoader(),
                new Class<?>[] {discovery}, (proxy, method, a) -> {
                    if (method.getDeclaringClass() == Object.class) return identity(proxy, method, a);
                    calls.add(method.getName() + (a != null && a[0] instanceof String ? " " + a[0] : ""));
                    return null;
                });
        Object onRegistration = Proxy.newProxyInstance(registration.getClassLoader(),
                new Class<?>[] {registration}, (proxy, method, a) -> {
                    if (method.getDeclaringClass() == Object.class) return identity(proxy, method, a);
                    calls.add(method.getName() + (a != null && a.length > 1 ? " " + a[1] : ""));
                    return null;
                });
        int dnsSd = manager.getField("PROTOCOL_DNS_SD").getInt(null);
        manager.getMethod("discoverServices", String.class, int.class,
                Class.forName("android.net.Network"), Executor.class, discovery)
                .invoke(nsd, "_gesher._tcp", dnsSd, null, now, onDiscovery);
        manager.getMethod("registerService", infoType, int.class, Executor.class, registration)
                .invoke(nsd, info, dnsSd, now, onRegistration);
        manager.getMethod("unregisterService", registration).invoke(nsd, onRegistration);
        manager.getMethod("stopServiceDiscovery", discovery).invoke(nsd, onDiscovery);
        require(calls.contains("onDiscoveryStarted _gesher._tcp"), "discovery did not start: " + calls);
        require(calls.contains("onRegistrationFailed 0"),
                "registering did not fail with FAILURE_INTERNAL_ERROR: " + calls);
        require(calls.size() == 2, "unexpected NSD callbacks: " + calls);
        System.out.println("FrameworkFixesTest: android.net.nsd.NsdManager: discovery starts, "
                + "registration fails through its listener, unregistering does not throw");
    }

    private static File zip(File file, String... entries) throws Exception {
        try (java.util.zip.ZipOutputStream out =
                new java.util.zip.ZipOutputStream(new java.io.FileOutputStream(file))) {
            for (String name : entries) {
                out.putNextEntry(new java.util.zip.ZipEntry(name));
                out.write(name.getBytes("UTF-8"));
                out.closeEntry();
            }
        }
        return file;
    }

    private static void splitAbi() throws Exception {
        String data = System.getenv("ANDROID_DATA");
        File dir = new File(data != null ? data : ".", "framework-fixes-splits");
        File libs = new File(dir, "lib");
        dir.mkdirs();
        try {
            String base = zip(new File(dir, "base.apk"), "classes.dex", "AndroidManifest.xml").getPath();
            String split = zip(new File(dir, "split_config.arm64_v8a.apk"),
                    "lib/arm64-v8a/libcheque.so", "lib/arm64-v8a/libc++_shared.so").getPath();
            String density = zip(new File(dir, "split_config.xxxhdpi.apk"), "res/a.png").getPath();
            Class<?> nl = Class.forName("com.gesher.app.NativeLibs");
            Method abiFor = nl.getMethod("abiFor", String[].class);
            Method forApk = nl.getMethod("forApk", String[].class);
            require(abiFor.invoke(null, (Object) new String[] {base}) == null,
                    "a base APK without libraries has an ABI");
            require("arm64-v8a".equals(abiFor.invoke(null, (Object) new String[] {base, density, split})),
                    "the ABI split's arm64-v8a is not the package's ABI");
            require("x86_64".equals(forApk.invoke(null, (Object) new String[] {base, density})),
                    "a package without native code does not run as x86_64");
            Method extract = nl.getMethod("extract", String.class, String.class, String.class);
            int n = 0;
            for (String apk : new String[] {base, density, split}) {
                n += (Integer) extract.invoke(null, apk, libs.getPath(), "arm64-v8a");
            }
            require(n == 2 && new File(libs, "libcheque.so").isFile()
                    && new File(libs, "libc++_shared.so").isFile(),
                    "the ABI split's libraries were not extracted (" + n + ")");
        } finally {
            File[] files = libs.listFiles();
            if (files != null) for (File f : files) f.delete();
            libs.delete();
            File[] apks = dir.listFiles();
            if (apks != null) for (File f : apks) f.delete();
            dir.delete();
        }
        System.out.println("FrameworkFixesTest: NativeLibs: the package ABI and libraries come "
                + "from its ABI split (config.arm64_v8a.apk), not base.apk alone");
    }

    private static void standInLists() throws Exception {
        Class<?> iface = Class.forName("android.content.pm.IShortcutService");
        Object impl = Class.forName("android.content.pm.IShortcutService$Default")
                .getConstructor().newInstance();
        Class<?> standIn = Class.forName("com.gesher.server.StandIn");
        Method of = standIn.getMethod("of", Class.class, Class.forName("android.os.IInterface"));
        Object service = standIn.getMethod("asInterface").invoke(of.invoke(null, iface, impl));
        Method getShortcuts = null;
        for (Method m : iface.getMethods()) {
            if (m.getName().equals("getShortcuts")) getShortcuts = m;
        }
        require(getShortcuts != null, "IShortcutService.getShortcuts is missing");
        Object[] a = new Object[getShortcuts.getParameterCount()];
        Class<?>[] types = getShortcuts.getParameterTypes();
        for (int i = 0; i < a.length; i++) a[i] = types[i] == int.class ? (Object) 0 : "gesher.test";
        Object slice = getShortcuts.invoke(service, a);
        require(slice != null, "an unimplemented getShortcuts answered null");
        Object list = slice.getClass().getMethod("getList").invoke(slice);
        require(list instanceof List && ((List<?>) list).isEmpty(),
                "an unimplemented getShortcuts did not answer an empty list: " + list);
        System.out.println("FrameworkFixesTest: a stand-in's unimplemented list method answers "
                + "an empty list (IShortcutService.getShortcuts), not null");
    }

    private static void htmlFromHtml() throws Exception {
        // On a device tagsoup is in the ext boot jar; Html.fromHtml's first instruction is
        // `new Parser()`, so without it every call fails, whatever the markup.
        Class.forName("org.ccil.cowan.tagsoup.Parser");
        Class.forName("org.ccil.cowan.tagsoup.HTMLSchema");
        // A bold end tag reads the font weight adjustment from the current application's
        // configuration: an app has one, this process none, so it gets one here, with the
        // system resources from $ANDROID_ROOT's framework-res.apk as gesher-app's Main has them.
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        Constructor<ActivityThread> newThread = ActivityThread.class.getDeclaredConstructor();
        newThread.setAccessible(true);
        ActivityThread thread = newThread.newInstance();
        Field initial = ActivityThread.class.getDeclaredField("mInitialApplication");
        initial.setAccessible(true);
        initial.set(thread, new Application() {
            @Override
            public Resources getResources() {
                return Resources.getSystem();
            }
        });
        Field current = ActivityThread.class.getDeclaredField("sCurrentActivityThread");
        current.setAccessible(true);
        current.set(null, thread);
        Spanned s = Html.fromHtml("<p><b>Bold</b> and <a href=\"https://example.org/\">a link</a>"
                + " &amp; &#x5e9;&#x5dc;&#x5d5;&#x5dd;</p>", Html.FROM_HTML_MODE_COMPACT);
        String text = s.toString().trim();
        require(text.equals("Bold and a link & שלום"),
                "fromHtml's text is wrong: '" + text + "'");
        StyleSpan[] bold = s.getSpans(0, s.length(), StyleSpan.class);
        require(bold.length == 1 && bold[0].getStyle() == Typeface.BOLD
                && s.getSpanStart(bold[0]) == text.indexOf("Bold")
                && s.getSpanEnd(bold[0]) == text.indexOf("Bold") + 4, "<b> is not one bold span");
        URLSpan[] links = s.getSpans(0, s.length(), URLSpan.class);
        require(links.length == 1 && "https://example.org/".equals(links[0].getURL())
                && text.substring(s.getSpanStart(links[0]), s.getSpanEnd(links[0])).equals("a link"),
                "<a href> is not one URLSpan over its text");
        System.out.println("FrameworkFixesTest: android.text.Html.fromHtml parses with tagsoup "
                + "(the ext boot jar's): text, entities, a bold span and a link");
    }

    public static void main(String[] args) throws Exception {
        // The FileChannel lock natives and Binder's (gesher-app loads them in an app).
        System.loadLibrary("android_runtime");
        colorMatrix();
        fileLocks();
        legacyRenderNode();
        nsd();
        splitAbi();
        standInLists();
        htmlFromHtml();
        System.out.println("FrameworkFixesTest all passed");
    }
}
