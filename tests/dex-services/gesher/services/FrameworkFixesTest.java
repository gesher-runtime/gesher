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

import com.android.i18n.phonenumbers.PhoneNumberMatch;
import com.android.i18n.phonenumbers.PhoneNumberUtil;
import com.android.i18n.phonenumbers.Phonenumber;

import java.io.File;
import java.io.RandomAccessFile;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executor;
import java.util.concurrent.TimeUnit;
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
 *  - android.net.DnsResolver (app/java, the Connectivity module's API): Waze died with
 *    NoClassDefFoundError when an address was chosen; query answers localhost's
 *    addresses through the executor, rawQuery a DNS response, other record types
 *    NOTIMP, and a cancelled query no callback;
 *  - android.bluetooth.BluetoothManager and BluetoothAdapter (app/java, commit
 *    ea735dee): Waze's beacon status died with NoClassDefFoundError; the adapter is
 *    null, as on a device without Bluetooth;
 *  - android.os.Debug's native heap counters (compat/framework/android_runtime.cc):
 *    Waze's startup died with UnsatisfiedLinkError on getNativeHeapSize.
 *  - android.bluetooth.BluetoothProfile (with ServiceListener), BluetoothHeadset and
 *    BluetoothDevice (app/java, the Bluetooth module's API on a device without
 *    Bluetooth): WhatsApp's headset listener implements ServiceListener and holds a
 *    BluetoothHeadset, and its worker died with NoClassDefFoundError resolving it.
 *  - libphonenumber (com.android.i18n.phonenumbers, the ext boot jar's; bundled into
 *    framework.jar with its metadata, tools/framework/plan.py's EXT_BUNDLED): Gmail's
 *    compose window died on a stackless NoClassDefFoundError for
 *    PhoneNumberUtil$PhoneNumberFormat when a recipient was typed.
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

    /** Runs a DnsResolver call whose last argument is its callback; the answer or error. */
    private static Object[] dnsCall(Class<?> callback, Method method, Object resolver,
            Object... args) throws Exception {
        Object[] result = new Object[3];
        CountDownLatch done = new CountDownLatch(1);
        Object cb = Proxy.newProxyInstance(callback.getClassLoader(), new Class<?>[] {callback},
                (proxy, m, a) -> {
                    if (m.getDeclaringClass() == Object.class) return identity(proxy, m, a);
                    result[0] = m.getName();
                    result[1] = a[0];
                    result[2] = a.length > 1 ? a[1] : null;
                    done.countDown();
                    return null;
                });
        Object[] all = java.util.Arrays.copyOf(args, args.length + 1);
        all[args.length] = cb;
        method.invoke(resolver, all);
        require(done.await(30, TimeUnit.SECONDS), method.getName() + ": no callback");
        return result;
    }

    private static void dnsResolver() throws Exception {
        Class<?> type = Class.forName("android.net.DnsResolver");
        Class<?> callback = Class.forName("android.net.DnsResolver$Callback");
        Class<?> network = Class.forName("android.net.Network");
        Class<?> signalType = Class.forName("android.os.CancellationSignal");
        Object resolver = type.getMethod("getInstance").invoke(null);
        int typeA = type.getField("TYPE_A").getInt(null);
        int classIn = type.getField("CLASS_IN").getInt(null);
        Method query = type.getMethod("query", network, String.class, int.class, int.class,
                Executor.class, signalType, callback);
        Method rawQuery = type.getMethod("rawQuery", network, String.class, int.class, int.class,
                int.class, Executor.class, signalType, callback);
        List<Thread> ran = new ArrayList<>();
        Executor executor = r -> {
            ran.add(Thread.currentThread());
            r.run();
        };

        Object[] a = dnsCall(callback, query, resolver, null, "localhost", typeA, 0, executor, null);
        require("onAnswer".equals(a[0]), "query localhost: " + a[0] + " " + a[1]);
        List<?> addresses = (List<?>) a[1];
        require((Integer) a[2] == 0, "query localhost: rcode " + a[2]);
        require(addresses.contains(java.net.InetAddress.getByAddress(new byte[] {127, 0, 0, 1})),
                "query localhost (TYPE_A): " + addresses);
        for (Object address : addresses) {
            require(address instanceof java.net.Inet4Address, "TYPE_A answered " + address);
        }
        require(ran.size() == 1, "the callback did not run on the executor");

        Object[] r = dnsCall(callback, rawQuery, resolver, null, "localhost", classIn, typeA, 0,
                executor, null);
        require("onAnswer".equals(r[0]), "rawQuery localhost: " + r[0] + " " + r[1]);
        byte[] response = (byte[]) r[1];
        require((response[2] & 0x80) != 0 && (response[3] & 0x0f) == 0 && (Integer) r[2] == 0,
                "rawQuery localhost: not a NOERROR response");
        int answers = ((response[6] & 0xff) << 8) | (response[7] & 0xff);
        require(answers >= 1, "rawQuery localhost: no answers");
        // One question (localhost: 11 bytes of name, type, class), then the first answer.
        int rdata = 12 + 11 + 4 + 12;
        require(response.length >= rdata + 4 && response[rdata] == 127 && response[rdata + 3] == 1,
                "rawQuery localhost: the first answer is not 127.0.0.1");

        Object[] mx = dnsCall(callback, rawQuery, resolver, null, "localhost", classIn, 15, 0,
                executor, null);
        require("onAnswer".equals(mx[0]) && (Integer) mx[2] == 4,
                "rawQuery MX: " + mx[0] + " rcode " + mx[2]);

        Object signal = signalType.getConstructor().newInstance();
        signalType.getMethod("cancel").invoke(signal);
        List<Object> late = new ArrayList<>();
        Object cb = Proxy.newProxyInstance(callback.getClassLoader(), new Class<?>[] {callback},
                (proxy, m, x) -> {
                    if (m.getDeclaringClass() == Object.class) return identity(proxy, m, x);
                    late.add(m.getName());
                    return null;
                });
        query.invoke(resolver, null, "localhost", typeA, 0, executor, signal, cb);
        Thread.sleep(500);
        require(late.isEmpty(), "a cancelled query called back: " + late);
        System.out.println("FrameworkFixesTest: android.net.DnsResolver: query and rawQuery answer "
                + "localhost through the executor, other types NOTIMP, a cancelled query is silent");
    }

    private static void bluetoothAndNativeHeap() throws Exception {
        Class<?> manager = Class.forName("android.bluetooth.BluetoothManager");
        Class<?> adapter = Class.forName("android.bluetooth.BluetoothAdapter");
        Object bluetooth = manager.getConstructor(Class.forName("android.content.Context"))
                .newInstance((Object) null);
        require(manager.getMethod("getAdapter").invoke(bluetooth) == null,
                "BluetoothManager.getAdapter() is not null");
        require(adapter.getMethod("getDefaultAdapter").invoke(null) == null,
                "BluetoothAdapter.getDefaultAdapter() is not null");
        long size = android.os.Debug.getNativeHeapSize();
        long allocated = android.os.Debug.getNativeHeapAllocatedSize();
        long free = android.os.Debug.getNativeHeapFreeSize();
        require(size > 0 && allocated > 0 && free >= 0 && allocated <= size,
                "native heap: size " + size + " allocated " + allocated + " free " + free);
        System.out.println("FrameworkFixesTest: BluetoothManager without an adapter; "
                + "Debug.getNativeHeapSize, getNativeHeapAllocatedSize and getNativeHeapFreeSize");
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

    private static void phoneNumbers() throws Exception {
        PhoneNumberUtil util = PhoneNumberUtil.getInstance();
        // Parsing reads the region's metadata from framework.jar's resources.
        Phonenumber.PhoneNumber us = util.parse("(650) 253-0000", "US");
        require(us.getCountryCode() == 1 && us.getNationalNumber() == 6502530000L,
                "parse gave " + us);
        require(util.isValidNumber(us), "+1 650-253-0000 is not valid");
        String e164 = util.format(us, PhoneNumberUtil.PhoneNumberFormat.E164);
        String intl = util.format(us, PhoneNumberUtil.PhoneNumberFormat.INTERNATIONAL);
        String national = util.format(us, PhoneNumberUtil.PhoneNumberFormat.NATIONAL);
        require(e164.equals("+16502530000") && intl.equals("+1 650-253-0000")
                && national.equals("(650) 253-0000"),
                "format gave " + e164 + " / " + intl + " / " + national);
        Phonenumber.PhoneNumber il = util.parse("050-123-4567", "IL");
        require(util.format(il, PhoneNumberUtil.PhoneNumberFormat.E164).equals("+972501234567"),
                "an Israeli number parsed as " + il);
        require(util.getRegionCodeForNumber(il).equals("IL"), "+972 50 is not IL");
        // The framework's API over it, as apps call it.
        String formatted = android.telephony.PhoneNumberUtils.formatNumber("6502530000", "US");
        require("(650) 253-0000".equals(formatted), "PhoneNumberUtils.formatNumber gave " + formatted);
        String toE164 = android.telephony.PhoneNumberUtils.formatNumberToE164("650-253-0000", "US");
        require("+16502530000".equals(toE164), "PhoneNumberUtils.formatNumberToE164 gave " + toE164);
        int found = 0;
        for (PhoneNumberMatch m : util.findNumbers("call 650-253-0000 today", "US")) {
            require(m.rawString().equals("650-253-0000"), "findNumbers found " + m.rawString());
            found++;
        }
        require(found == 1, "findNumbers found " + found + " numbers");
        System.out.println("FrameworkFixesTest: libphonenumber on the boot class path: parse, format "
                + "(E164, INTERNATIONAL, NATIONAL), findNumbers and PhoneNumberUtils.formatNumber");
    }

    private static void bluetoothProfiles() throws Exception {
        Class<?> profile = Class.forName("android.bluetooth.BluetoothProfile");
        Class<?> listener = Class.forName("android.bluetooth.BluetoothProfile$ServiceListener");
        Class<?> headset = Class.forName("android.bluetooth.BluetoothHeadset");
        Class<?> device = Class.forName("android.bluetooth.BluetoothDevice");
        require(profile.isInterface() && listener.isInterface(), "BluetoothProfile types are not interfaces");
        require(profile.isAssignableFrom(headset), "BluetoothHeadset is not a BluetoothProfile");
        require(profile.getField("HEADSET").getInt(null) == 1
                && headset.getField("STATE_AUDIO_CONNECTED").getInt(null) == 12
                && device.getField("BOND_BONDED").getInt(null) == 12, "Bluetooth constants are wrong");
        // An app's listener, as WhatsApp's implements it; with no adapter it is never called.
        Object app = Proxy.newProxyInstance(listener.getClassLoader(), new Class<?>[] {listener},
                (proxy, method, args) -> null);
        listener.getMethod("onServiceDisconnected", int.class).invoke(app, 1);
        listener.getMethod("onServiceConnected", int.class, profile).invoke(app, 1, null);
        require(headset.getMethod("isAudioConnected", device) != null, "isAudioConnected is missing");
        System.out.println("FrameworkFixesTest: BluetoothProfile, its ServiceListener, BluetoothHeadset "
                + "and BluetoothDevice resolve on a device without Bluetooth");
    }

    private static void handlerThreadQuit() throws Exception {
        // MessageQueue.quit builds a Perfetto counter event: its classes' natives
        // (PerfettoTrackEventExtra and the nested ones) must be there with tracing off.
        // Gmail's emojiCompat thread died on native_delete at quit.
        android.os.HandlerThread thread = new android.os.HandlerThread("fixes-quit");
        thread.start();
        java.util.concurrent.CountDownLatch ran = new java.util.concurrent.CountDownLatch(1);
        new android.os.Handler(thread.getLooper()).post(ran::countDown);
        require(ran.await(10, java.util.concurrent.TimeUnit.SECONDS), "the handler thread did not run");
        require(thread.quit(), "HandlerThread.quit returned false");
        thread.join(10_000);
        require(!thread.isAlive(), "the handler thread did not end");
        for (String nested : new String[] {"", "$ArgInt64", "$ArgBool", "$ArgDouble", "$ArgString",
                "$FieldInt64", "$FieldDouble", "$FieldString", "$FieldNested", "$Proto", "$Flow",
                "$NamedTrack", "$CounterTrack", "$CounterInt64", "$CounterDouble"}) {
            Class.forName("android.os.PerfettoTrackEventExtra" + nested, true, null);
        }
        System.out.println("FrameworkFixesTest: HandlerThread.quit after a message, and every "
                + "PerfettoTrackEventExtra class initializes with tracing off");
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
        dnsResolver();
        bluetoothAndNativeHeap();
        bluetoothProfiles();
        phoneNumbers();
        handlerThreadQuit();
        System.out.println("FrameworkFixesTest all passed");
    }
}
