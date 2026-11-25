/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.content.pm.FeatureInfo;
import android.content.pm.IDexModuleRegisterCallback;
import android.content.pm.PackageManager;
import android.os.RemoteException;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.PackageManagerService;
import java.lang.reflect.Field;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executor;
import sun.misc.Unsafe;

/** Host/ART production-method checks; real PackageManager/Parcel tests live beside these. */
public final class PackageServiceQueriesTest {
    private static int checks;

    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }

    /**
     * The reported system features follow what backs them (docs/system-features.md):
     * the production list matches the probes this process can make, and the helpers claim
     * a feature only for the hardware or runtime that implements it.
     */
    /**
     * The system server's OpenGL ES probe is bounded (PackageManagerService.boundedProbe):
     * an answer passes through, a probe that hangs gives -2 (no version declared) after the
     * timeout instead of blocking the start, and a LinkageError reaches the caller.
     */
    /**
     * PackageManagerService.lateGlEsVersion: a late answer is declared once nothing was;
     * a known version stays. Run only where it cannot write ro.opengles.version for real
     * (no property natives, or the property already set); the fields are restored.
     */
    private static void checkLateGlesVersion() throws ReflectiveOperationException {
        boolean propertySet;
        int property;
        try {
            property = android.os.SystemProperties.getInt("ro.opengles.version", 0);
            propertySet = property != 0;
        } catch (LinkageError e) {
            property = 0;
            propertySet = true;  // no property natives: nothing can be written
        }
        if (!propertySet) return;
        // A late answer is declared; where the boot already declared a version, the
        // declaration stays equal to the property.
        final int expected = property == 0x30000 || property == 0x20000 ? property : 0x30000;
        Field known = PackageManagerService.class.getDeclaredField("sGlEsVersionKnown");
        Field version = PackageManagerService.class.getDeclaredField("sGlEsVersion");
        known.setAccessible(true);
        version.setAccessible(true);
        java.lang.reflect.Method late = PackageManagerService.class.getDeclaredMethod("lateGlEsVersion", int.class);
        late.setAccessible(true);
        synchronized (PackageManagerService.class) {
            boolean savedKnown = known.getBoolean(null);
            int savedVersion = version.getInt(null);
            try {
                known.setBoolean(null, true);
                version.setInt(null, FeatureInfo.GL_ES_VERSION_UNDEFINED);
                late.invoke(null, 0x30000);
                require(PackageManagerService.glEsVersion() == expected,
                        "a late GLES answer was not declared as 0x" + Integer.toHexString(expected));
                late.invoke(null, 0x20000);
                require(PackageManagerService.glEsVersion() == expected,
                        "a second late GLES answer replaced a known version");
            } finally {
                known.setBoolean(null, savedKnown);
                version.setInt(null, savedVersion);
            }
        }
    }

    private static void checkBoundedGlesProbe() {
        try {
            java.lang.reflect.Method bounded = PackageManagerService.class.getDeclaredMethod(
                    "boundedProbe", java.util.function.IntSupplier.class, long.class,
                    java.util.function.IntConsumer.class);
            bounded.setAccessible(true);
            final int[] late = {0};
            final java.util.concurrent.CountDownLatch lateAnswer = new java.util.concurrent.CountDownLatch(1);
            java.util.function.IntConsumer onLate = v -> {
                late[0] = v;
                lateAnswer.countDown();
            };
            require((Integer) bounded.invoke(null,
                            (java.util.function.IntSupplier) () -> 0x20000, 5000L, onLate) == 0x20000
                            && late[0] == 0,
                    "bounded GLES probe did not pass its answer through (or called late)");
            java.util.concurrent.CountDownLatch release = new java.util.concurrent.CountDownLatch(1);
            long start = System.nanoTime();
            int hung = (Integer) bounded.invoke(null, (java.util.function.IntSupplier) () -> {
                try {
                    release.await();
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
                return 0x30000;
            }, 200L, onLate);
            long millis = (System.nanoTime() - start) / 1_000_000;
            require(hung == -2 && millis >= 150 && millis < 3000,
                    "hung GLES probe gave " + hung + " after " + millis + " ms");
            require(late[0] == 0, "late answer delivered before the probe answered");
            release.countDown();
            require(lateAnswer.await(5, java.util.concurrent.TimeUnit.SECONDS) && late[0] == 0x30000,
                    "the answer after the timeout did not reach the late handler: 0x"
                            + Integer.toHexString(late[0]));
            boolean thrown = false;
            try {
                bounded.invoke(null, (java.util.function.IntSupplier) () -> {
                    throw new UnsatisfiedLinkError("no natives");
                }, 5000L, onLate);
            } catch (java.lang.reflect.InvocationTargetException e) {
                thrown = e.getCause() instanceof UnsatisfiedLinkError;
            }
            require(thrown, "bounded GLES probe swallowed a LinkageError");
            checkLateGlesVersion();
            System.out.println("system features: bounded GLES probe: answer, timeout (" + millis
                    + " ms), the late answer, and LinkageError");
        } catch (ReflectiveOperationException | InterruptedException e) {
            throw new AssertionError("PackageManagerService.boundedProbe: " + e, e);
        }
    }

    /**
     * build.prop loading keeps ro.opengles.version only as a version Gesher serves
     * (BuildProps.servedGlEsVersion; compat/art/jni_invocation_static.cc does the same):
     * 0x30000 and 0x20000 in any integer form; 0x30001 or garbage is left unset, so the
     * probe declares the version. Other properties are not affected.
     */
    private static void checkBuildPropGlesVersion() {
        try {
            Class<?> props = Class.forName("com.gesher.app.BuildProps");
            java.lang.reflect.Method served = props.getDeclaredMethod("servedGlEsVersion", String.class, String.class);
            served.setAccessible(true);
            String key = "ro.opengles.version";
            // The same table as tests/native/egl_gles_version_test.cc (gles_version_property.h).
            for (String value : new String[] {"196608", "131072", "0x30000", "0X30000", "0x20000"}) {
                require((Boolean) served.invoke(null, key, value),
                        "build.prop: the served OpenGL ES version \"" + value + "\" was refused");
            }
            for (String value : new String[] {"196609", "0x30001", "65536", "#30000", "+196608", "-196608", " 196608", "196608 ", "0x", "", "three", "0x3000G", "1966080000000", "0196608", "00x30000"}) {
                require(!(Boolean) served.invoke(null, key, value),
                        "build.prop: \"" + value + "\" was kept as an OpenGL ES version");
            }
            require((Boolean) served.invoke(null, "ro.product.model", "196609"),
                    "build.prop: another property was filtered");
        } catch (ReflectiveOperationException e) {
            throw new AssertionError("BuildProps.servedGlEsVersion: " + e, e);
        }
    }

    /**
     * The boot probe (PackageManagerService's static initializer) answers within its bound
     * in the normal case: its thread must not wait for that initialization to finish
     * (boundedProbe's ProbeRun). Only where it ran (the property natives present).
     */
    /** Whether this process's boot probe ran (no preset ro.opengles.version declared). */
    private static boolean bootProbeRan() {
        try {
            Field ran = PackageManagerService.class.getDeclaredField("sGlProbeRan");
            ran.setAccessible(true);
            return ran.getBoolean(null);
        } catch (ReflectiveOperationException e) {
            throw new AssertionError("PackageManagerService.sGlProbeRan: " + e, e);
        }
    }

    private static void checkBootProbeBounded() {
        try {
            Field ran = PackageManagerService.class.getDeclaredField("sGlProbeRan");
            Field timedOut = PackageManagerService.class.getDeclaredField("sGlProbeTimedOut");
            Field millis = PackageManagerService.class.getDeclaredField("sGlProbeMillis");
            Field bound = PackageManagerService.class.getDeclaredField("GL_PROBE_TIMEOUT_MS");
            ran.setAccessible(true);
            timedOut.setAccessible(true);
            millis.setAccessible(true);
            bound.setAccessible(true);
            if (!ran.getBoolean(null)) return;
            long took = millis.getLong(null);
            System.out.println("system features: the boot's OpenGL ES probe answered in " + took + " ms");
            require(!timedOut.getBoolean(null) && took < bound.getLong(null),
                    "the boot's OpenGL ES probe did not answer within its bound (" + took + " ms)");
        } catch (ReflectiveOperationException e) {
            throw new AssertionError("PackageManagerService probe fields: " + e, e);
        }
    }

    private static void checkSystemFeatures(PackageManagerService service) {
        checkBootProbeBounded();
        checkBuildPropGlesVersion();
        List<FeatureInfo> list = service.getSystemAvailableFeatures().getList();
        // CI runs this class without libandroid_runtime (no property natives); through
        // ServiceFixtureRunner it has them. Each expectation follows what this process has.
        Boolean lowRam;
        int expectedGles;
        try {
            expectedGles = android.os.SystemProperties.getInt(
                    "ro.opengles.version", FeatureInfo.GL_ES_VERSION_UNDEFINED);
            lowRam = android.app.ActivityManager.isLowRamDeviceStatic();
        } catch (LinkageError e) {
            expectedGles = FeatureInfo.GL_ES_VERSION_UNDEFINED;
            lowRam = null;
        }
        int unnamed = 0;
        for (FeatureInfo feature : list) {
            if (feature.name != null) {
                require(service.hasSystemFeature(feature.name, feature.version),
                        "Listed feature " + feature.name + " is not reported by hasSystemFeature");
                continue;
            }
            unnamed++;
            require(feature.reqGlEsVersion == PackageManagerService.glEsVersion()
                            && feature.reqGlEsVersion == expectedGles,
                    "OpenGL ES entry does not carry ro.opengles.version");
        }
        require(unnamed == 1, "Available features need exactly one OpenGL ES entry, got " + unnamed);
        checkBoundedGlesProbe();
        // The version is the default EGL display's (compat/bionic/egl_gles_version.h): ES 3.0
        // only on a hardware Direct3D 11 device, never on WARP. -1 without the natives.
        int probed;
        try {
            probed = com.gesher.server.WindowsHost.glEsVersion();
        } catch (LinkageError e) {
            probed = -1;
        }
        if (probed >= 0) {
            require(PackageManagerService.glEsVersion() == probed,
                    "OpenGL ES entry 0x" + Integer.toHexString(PackageManagerService.glEsVersion())
                            + " is not the default display's 0x" + Integer.toHexString(probed));
            // On WARP the boot's own probe declares 2.0; a preset declaration (a build.prop
            // with 0x30000) is given as declared instead, by WARP uncapped.
            if (probed > 0 && "warp".equals(System.getenv("GESHER_EGL_DEVICE")) && bootProbeRan()) {
                require(probed == 0x20000, "WARP declared OpenGL ES 0x" + Integer.toHexString(probed)
                        + ", not 2.0");
            }
        }
        // ApplicationSharedMemory's cache takes the same list, the unnamed entry included.
        new android.content.pm.SystemFeaturesCache(list);

        require(service.hasSystemFeature(PackageManager.FEATURE_RAM_NORMAL, 0)
                                == Boolean.FALSE.equals(lowRam)
                        && service.hasSystemFeature(PackageManager.FEATURE_RAM_LOW, 0)
                                == Boolean.TRUE.equals(lowRam),
                "RAM features disagree with ActivityManager.isLowRamDevice (" + lowRam + ")");

        String webView;
        String webViewError;
        try {
            webView = com.gesher.app.webview.WebView2Host.runtimeVersion();
            webViewError = webView == null ? com.gesher.app.webview.WebView2Host.runtimeError() : null;
        } catch (LinkageError e) {
            webView = null;
            webViewError = String.valueOf(e);
        }
        if (webViewError != null) System.out.println("system features: no WebView2: " + webViewError);
        require(service.hasSystemFeature(PackageManager.FEATURE_WEBVIEW, 0) == (webView != null),
                "WebView feature disagrees with the WebView2 Runtime (" + webView + ")");

        int[] hardware;
        try {
            hardware = com.gesher.server.WindowsHost.hardwareSummary();
        } catch (LinkageError e) {
            hardware = null;
        }
        int contacts = hardware != null ? hardware[0] : 0;
        boolean audio = hardware != null && hardware[1] > 0;
        boolean ethernet = hardware != null && hardware[2] > 0;
        require(service.hasSystemFeature(PackageManager.FEATURE_TOUCHSCREEN_MULTITOUCH, 0)
                                == (contacts >= 2)
                        && service.hasSystemFeature(
                                PackageManager.FEATURE_TOUCHSCREEN_MULTITOUCH_DISTINCT, 0)
                                == (contacts >= 2)
                        && service.hasSystemFeature(
                                PackageManager.FEATURE_TOUCHSCREEN_MULTITOUCH_JAZZHAND, 0)
                                == (contacts >= 5),
                "Multitouch features disagree with the touch digitizer's " + contacts + " contacts");
        require(service.hasSystemFeature(PackageManager.FEATURE_AUDIO_OUTPUT, 0) == audio,
                "Audio output feature disagrees with the render endpoints");
        require(service.hasSystemFeature(PackageManager.FEATURE_ETHERNET, 0) == ethernet,
                "Ethernet feature disagrees with the wired Ethernet interfaces");
        System.out.println("system features: " + list.size() + " entries, GLES 0x"
                + Integer.toHexString(PackageManagerService.glEsVersion()) + ", webview "
                + webView + ", hardware " + Arrays.toString(hardware));

        // Never claimed: no backend implements them (docs/system-features.md).
        for (String absent : new String[] {
                PackageManager.FEATURE_OPENGLES_EXTENSION_PACK,
                PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL,
                PackageManager.FEATURE_VULKAN_HARDWARE_VERSION,
                PackageManager.FEATURE_PC,
                PackageManager.FEATURE_INPUT_METHODS,
                PackageManager.FEATURE_HOME_SCREEN,
                PackageManager.FEATURE_PICTURE_IN_PICTURE,
                PackageManager.FEATURE_AUDIO_LOW_LATENCY,
                PackageManager.FEATURE_CAMERA_EXTERNAL,
                PackageManager.FEATURE_CAMERA_FLASH,
                PackageManager.FEATURE_USB_HOST,
                PackageManager.FEATURE_NFC,
                PackageManager.FEATURE_SECURE_LOCK_SCREEN,
                PackageManager.FEATURE_VERIFIED_BOOT}) {
            require(!service.hasSystemFeature(absent, 0), "Unimplemented feature claimed: " + absent);
        }
        // Cameras: FEATURE_CAMERA for one configured as rear, FEATURE_CAMERA_FRONT for the
        // others (a PC's camera faces the user, CONTRIBUTING.md); none without natives.
        List<com.gesher.server.WindowsCamera.Device> cameras;
        try {
            cameras = com.gesher.server.WindowsCamera.devices();
        } catch (LinkageError e) {
            cameras = Collections.emptyList();
        }
        boolean rearCamera = false;
        boolean frontCamera = false;
        boolean rearAutofocus = false;
        for (com.gesher.server.WindowsCamera.Device camera : cameras) {
            if (camera.isRearFacing()) {
                rearCamera = true;
                rearAutofocus |= camera.autofocus;
            } else {
                frontCamera = true;
            }
        }
        // FEATURE_CAMERA_AUTOFOCUS: only a rear camera's real autofocus (CameraAutofocusContractTest).
        require(service.hasSystemFeature(PackageManager.FEATURE_CAMERA_ANY, 0) == !cameras.isEmpty()
                        && service.hasSystemFeature(PackageManager.FEATURE_CAMERA, 0) == rearCamera
                        && service.hasSystemFeature(PackageManager.FEATURE_CAMERA_FRONT, 0)
                                == frontCamera
                        && service.hasSystemFeature(PackageManager.FEATURE_CAMERA_AUTOFOCUS, 0)
                                == rearAutofocus,
                "Camera features disagree with the " + cameras.size() + " Windows cameras");

        android.util.ArrayMap<String, FeatureInfo> f = new android.util.ArrayMap<>();
        PackageManagerService.addHostHardwareFeatures(f, null);
        PackageManagerService.addHostHardwareFeatures(f, new int[] {-1, -1, -1});
        PackageManagerService.addHostHardwareFeatures(f, new int[] {1, 0, 0});
        require(f.isEmpty(), "Hardware features claimed without hardware: " + f.keySet());
        PackageManagerService.addHostHardwareFeatures(f, new int[] {2, 1, 1});
        require(f.containsKey(PackageManager.FEATURE_TOUCHSCREEN_MULTITOUCH)
                        && f.containsKey(PackageManager.FEATURE_TOUCHSCREEN_MULTITOUCH_DISTINCT)
                        && !f.containsKey(PackageManager.FEATURE_TOUCHSCREEN_MULTITOUCH_JAZZHAND)
                        && f.containsKey(PackageManager.FEATURE_AUDIO_OUTPUT)
                        && f.containsKey(PackageManager.FEATURE_ETHERNET) && f.size() == 4,
                "Two-contact digitizer, endpoint and interface: " + f.keySet());
        f.clear();
        PackageManagerService.addHostHardwareFeatures(f, new int[] {10, 0, 0});
        require(f.containsKey(PackageManager.FEATURE_TOUCHSCREEN_MULTITOUCH_JAZZHAND) && f.size() == 3,
                "Ten-contact digitizer: " + f.keySet());
        f.clear();
        PackageManagerService.addWebViewFeature(f, null);
        PackageManagerService.addWebViewFeature(f, "");
        require(f.isEmpty(), "WebView claimed without a WebView2 Runtime");
        PackageManagerService.addWebViewFeature(f, "131.0.2903.112");
        require(f.containsKey(PackageManager.FEATURE_WEBVIEW), "WebView omitted with a runtime");
        f.clear();
        PackageManagerService.addMemoryFeature(f, true);
        require(f.containsKey(PackageManager.FEATURE_RAM_LOW) && f.size() == 1, "low-RAM device");
        f.clear();
        PackageManagerService.addMemoryFeature(f, false);
        require(f.containsKey(PackageManager.FEATURE_RAM_NORMAL) && f.size() == 1, "normal-RAM device");
    }

    /** Gesher's stationary accelerometer's name (compat/devices/sensors.cc, docs/devices.md). */
    private static final String STATIONARY_ACCELEROMETER = "Gesher Stationary Accelerometer";

    /**
     * Every sensor SystemSensorManager's natives list in this process, as a SensorManager
     * built on them would; empty without the runtime natives (the plain run).
     */
    private static List<android.hardware.Sensor> nativeSensorList() throws Exception {
        List<android.hardware.Sensor> list = new ArrayList<>();
        Class<?> manager = android.hardware.SystemSensorManager.class;
        java.lang.reflect.Method classInit = manager.getDeclaredMethod("nativeClassInit");
        java.lang.reflect.Method create = manager.getDeclaredMethod("nativeCreate", String.class);
        java.lang.reflect.Method at = manager.getDeclaredMethod("nativeGetSensorAtIndex",
                long.class, android.hardware.Sensor.class, int.class);
        java.lang.reflect.Constructor<android.hardware.Sensor> constructor =
                android.hardware.Sensor.class.getDeclaredConstructor();
        classInit.setAccessible(true);
        create.setAccessible(true);
        at.setAccessible(true);
        constructor.setAccessible(true);
        long handle;
        try {
            classInit.invoke(null);
            handle = (Long) create.invoke(null, "android");
        } catch (java.lang.reflect.InvocationTargetException e) {
            if (e.getCause() instanceof LinkageError) return list; // no runtime natives
            throw e;
        } catch (LinkageError e) {
            return list; // no runtime natives: SystemSensorManager cannot initialize
        }
        for (int i = 0; ; i++) {
            android.hardware.Sensor sensor = constructor.newInstance();
            if (!(Boolean) at.invoke(null, handle, sensor, i)) break;
            list.add(sensor);
        }
        return list;
    }

    private static void field(Object target, String name, Object value) throws Exception {
        Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(target, value);
    }

    private static final class QueueExecutor implements Executor {
        final ArrayDeque<Runnable> work = new ArrayDeque<>();
        @Override public void execute(Runnable task) { work.addLast(task); }
        void runOne() { work.removeFirst().run(); }
    }

    private static ApkPackage record(Unsafe unsafe, String name, int uid,
            String... permissions) throws Exception {
        ApkPackage pkg = (ApkPackage) unsafe.allocateInstance(ApkPackage.class);
        field(pkg, "packageName", name);
        field(pkg, "mUid", uid);
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(name, "/fixture/" + name + ".apk");
        parsed.setTargetSdkVersion(android.os.Build.VERSION_CODES.O);
        for (String permission : permissions) {
            parsed.addUsesPermission(
                    new ParsedUsesPermissionImpl(permission, 0, Collections.emptySet()));
        }
        field(pkg, "parsed", parsed);
        return pkg;
    }

    public static void main(String[] args) throws Exception {
        Field singleton = Unsafe.class.getDeclaredField("theUnsafe");
        singleton.setAccessible(true);
        Unsafe unsafe = (Unsafe) singleton.get(null);
        // The methods below use package identities and an executor, not the
        // native-backed installer/permission services built by the constructor.
        PackageManagerService service = (PackageManagerService) unsafe.allocateInstance(
                PackageManagerService.class);
        Map<String, ApkPackage> packages = new LinkedHashMap<>();
        packages.put("fixture.one", record(unsafe, "fixture.one", 12001,
                Manifest.permission.REQUEST_INSTALL_PACKAGES));
        packages.put("fixture.two", record(unsafe, "fixture.two", 12002));
        ApkPackage addon = record(unsafe, "fixture.system", 12003);
        field(addon, "mSystemAddon", true);
        packages.put("fixture.system", addon);
        field(service, "mPackages", packages);
        Map<String, ApkPackage> removing = new LinkedHashMap<>();
        field(service, "mRemoving", removing);
        QueueExecutor executor = new QueueExecutor();
        field(service, "mPackageEventExecutor", executor);

        require(service.hasSystemFeature(PackageManager.FEATURE_TOUCHSCREEN, 0),
                "Device did not advertise touchscreen feature");
        boolean listedTouchscreen = false;
        for (FeatureInfo feature : service.getSystemAvailableFeatures().getList()) {
            if (PackageManager.FEATURE_TOUCHSCREEN.equals(feature.name)) listedTouchscreen = true;
        }
        require(listedTouchscreen, "Available features list omitted touchscreen");
        // The OpenGL ES entry (one unnamed entry carrying ro.opengles.version, as AOSP's
        // list) is checked by checkSystemFeatures, with or without the property natives.
        // The network location provider is always registered; GPS follows a real GNSS
        // receiver exactly (none without the runtime natives).
        require(service.hasSystemFeature(PackageManager.FEATURE_LOCATION, 0)
                        && service.hasSystemFeature(PackageManager.FEATURE_LOCATION_NETWORK, 0)
                        && service.hasSystemFeature(PackageManager.FEATURE_LOCATION_GPS, 0)
                                == com.gesher.server.WindowsLocation.hasGnssReceiver(),
                "Location features do not match the providers and the GNSS receiver");
        for (boolean gnss : new boolean[] {false, true}) {
            android.util.ArrayMap<String, FeatureInfo> located = new android.util.ArrayMap<>();
            PackageManagerService.addLocationFeatures(located, gnss);
            require(located.containsKey(PackageManager.FEATURE_LOCATION)
                            && located.containsKey(PackageManager.FEATURE_LOCATION_NETWORK)
                            && located.containsKey(PackageManager.FEATURE_LOCATION_GPS) == gnss,
                    "FEATURE_LOCATION_GPS does not follow the receiver (" + gnss + ")");
        }
        // FEATURE_MICROPHONE follows the audio input exactly, as this process's
        // AudioSystem reports it (none where its natives are unavailable).
        boolean input;
        try {
            android.util.IntArray inputs = new android.util.IntArray();
            input = android.media.AudioSystem.getSupportedDeviceTypes(
                    android.media.AudioManager.GET_DEVICES_INPUTS, inputs)
                    == android.media.AudioSystem.SUCCESS && inputs.size() > 0;
        } catch (LinkageError e) {
            input = false;
        }
        require(service.hasSystemFeature(PackageManager.FEATURE_MICROPHONE, 0) == input,
                "Microphone feature disagrees with the audio input devices");
        android.util.ArrayMap<String, FeatureInfo> mic = new android.util.ArrayMap<>();
        PackageManagerService.addMicrophoneFeature(mic, false);
        require(mic.isEmpty(), "Microphone claimed without an input device");
        PackageManagerService.addMicrophoneFeature(mic, true);
        require(mic.containsKey(PackageManager.FEATURE_MICROPHONE),
                "Microphone omitted with an input device");
        // FEATURE_SENSOR_* follow the sensors SensorManager lists in this process.
        java.util.Set<Integer> sensorTypes = new java.util.HashSet<>();
        for (int type : com.gesher.server.WindowsSensors.types()) sensorTypes.add(type);
        System.out.println("PackageServiceQueriesTest: sensor types " + sensorTypes);
        require(service.hasSystemFeature(PackageManager.FEATURE_SENSOR_ACCELEROMETER, 0)
                        == sensorTypes.contains(android.hardware.Sensor.TYPE_ACCELEROMETER)
                        && service.hasSystemFeature(PackageManager.FEATURE_SENSOR_GYROSCOPE, 0)
                        == sensorTypes.contains(android.hardware.Sensor.TYPE_GYROSCOPE)
                        && service.hasSystemFeature(PackageManager.FEATURE_SENSOR_COMPASS, 0)
                        == sensorTypes.contains(android.hardware.Sensor.TYPE_MAGNETIC_FIELD)
                        && service.hasSystemFeature(PackageManager.FEATURE_SENSOR_LIGHT, 0)
                        == sensorTypes.contains(android.hardware.Sensor.TYPE_LIGHT),
                "Sensor features disagree with the listed sensors");
        // Without sensors (the CI runners), or without the runtime natives (this class run
        // as a main class), the comparison above holds even if nothing were wired.
        // "sensor-test" (run through ServiceFixtureRunner, so the natives are registered,
        // with GESHER_SENSOR_TEST set) requires the test accelerometer and light sensor to
        // reach FEATURES through the production path: natives, WindowsSensors, clinit.
        if (Arrays.asList(args).contains("sensor-test")) {
            require(sensorTypes.contains(android.hardware.Sensor.TYPE_ACCELEROMETER)
                            && sensorTypes.contains(android.hardware.Sensor.TYPE_LIGHT),
                    "GESHER_SENSOR_TEST's sensors are not listed: " + sensorTypes);
            require(service.hasSystemFeature(PackageManager.FEATURE_SENSOR_ACCELEROMETER, 0)
                            && service.hasSystemFeature(PackageManager.FEATURE_SENSOR_LIGHT, 0),
                    "GESHER_SENSOR_TEST's sensors are not system features");
            boolean accelerometer = false;
            boolean light = false;
            for (FeatureInfo f : service.getSystemAvailableFeatures().getList()) {
                accelerometer |= PackageManager.FEATURE_SENSOR_ACCELEROMETER.equals(f.name);
                light |= PackageManager.FEATURE_SENSOR_LIGHT.equals(f.name);
            }
            require(accelerometer && light,
                    "getSystemAvailableFeatures omits GESHER_SENSOR_TEST's sensors");
            System.out.println("PackageServiceQueriesTest: GESHER_SENSOR_TEST's accelerometer"
                    + " and light sensor are system features");
        }
        // Gesher's stationary accelerometer (GESHER_SENSOR_FALLBACK, on unless "0") is in
        // SensorManager's native list on a PC without Windows motion sensors (the runners),
        // but it is a desktop default, not hardware: WindowsSensors.types() leaves it out
        // and no FEATURE_SENSOR_* comes from it. "sensor-fallback" requires it listed and
        // featureless; "sensor-fallback-off" (GESHER_SENSOR_FALLBACK=0) requires it absent.
        List<android.hardware.Sensor> listed = nativeSensorList();
        int stationary = 0;
        int listedAccelerometers = 0;
        for (android.hardware.Sensor sensor : listed) {
            if (sensor.getType() != android.hardware.Sensor.TYPE_ACCELEROMETER) continue;
            listedAccelerometers++;
            if (STATIONARY_ACCELEROMETER.equals(sensor.getName())) stationary++;
        }
        System.out.println("PackageServiceQueriesTest: SensorManager lists " + listed.size()
                + " sensors, " + stationary + " stationary accelerometer");
        int typedAccelerometers = 0;
        for (int type : com.gesher.server.WindowsSensors.types()) {
            if (type == android.hardware.Sensor.TYPE_ACCELEROMETER) typedAccelerometers++;
        }
        require(typedAccelerometers == listedAccelerometers - stationary,
                "WindowsSensors.types() does not give exactly the non-stationary accelerometers: "
                        + typedAccelerometers + " of " + listedAccelerometers + " listed, "
                        + stationary + " stationary");
        require(service.hasSystemFeature(PackageManager.FEATURE_SENSOR_ACCELEROMETER, 0)
                        == (listedAccelerometers > stationary),
                "Accelerometer feature disagrees with the real accelerometers listed");
        if (Arrays.asList(args).contains("sensor-fallback")) {
            require(stationary == 1 && listedAccelerometers == 1,
                    "The stationary accelerometer is not the only accelerometer listed: "
                            + listedAccelerometers + " accelerometers, " + stationary
                            + " stationary");
            require(!service.hasSystemFeature(PackageManager.FEATURE_SENSOR_ACCELEROMETER, 0),
                    "The stationary accelerometer declared FEATURE_SENSOR_ACCELEROMETER");
            for (FeatureInfo f : service.getSystemAvailableFeatures().getList()) {
                require(f.name == null || !f.name.startsWith("android.hardware.sensor."),
                        "The stationary accelerometer declared a sensor feature: " + f.name);
            }
            System.out.println("PackageServiceQueriesTest: the stationary accelerometer is"
                    + " listed and declares no system feature");
        }
        if (Arrays.asList(args).contains("sensor-fallback-off")) {
            require(stationary == 0, "GESHER_SENSOR_FALLBACK=0 still lists the stationary"
                    + " accelerometer");
            require(!service.hasSystemFeature(PackageManager.FEATURE_SENSOR_ACCELEROMETER, 0)
                            || listedAccelerometers > 0,
                    "Accelerometer feature without an accelerometer");
            System.out.println("PackageServiceQueriesTest: GESHER_SENSOR_FALLBACK=0 lists no"
                    + " stationary accelerometer and its features follow the real sensors");
        }
        android.util.ArrayMap<String, FeatureInfo> sensors = new android.util.ArrayMap<>();
        PackageManagerService.addSensorFeatures(sensors, new int[0]);
        PackageManagerService.addSensorFeatures(sensors, null);
        require(sensors.isEmpty(), "Sensor feature claimed without a sensor");
        PackageManagerService.addSensorFeatures(sensors, new int[] {
                android.hardware.Sensor.TYPE_ROTATION_VECTOR});
        require(sensors.isEmpty(), "A rotation vector alone claimed a sensor feature");
        PackageManagerService.addSensorFeatures(sensors, new int[] {
                android.hardware.Sensor.TYPE_ACCELEROMETER, android.hardware.Sensor.TYPE_LIGHT});
        require(sensors.size() == 2
                        && sensors.containsKey(PackageManager.FEATURE_SENSOR_ACCELEROMETER)
                        && sensors.containsKey(PackageManager.FEATURE_SENSOR_LIGHT),
                "Sensor features do not match an accelerometer and a light sensor");
        PackageManagerService.addSensorFeatures(sensors, new int[] {
                android.hardware.Sensor.TYPE_GYROSCOPE, android.hardware.Sensor.TYPE_MAGNETIC_FIELD});
        require(sensors.size() == 4
                        && sensors.containsKey(PackageManager.FEATURE_SENSOR_GYROSCOPE)
                        && sensors.containsKey(PackageManager.FEATURE_SENSOR_COMPASS),
                "Sensor features do not match a gyroscope and a magnetometer");
        checkSystemFeatures(service);
        require(service.canRequestPackageInstalls("fixture.one", 0),
                "Package declaring REQUEST_INSTALL_PACKAGES could not request installs");
        PackageImpl installer = (PackageImpl) packages.get("fixture.one").parsed;
        installer.setTargetSdkVersion(android.os.Build.VERSION_CODES.O - 1);
        require(!service.canRequestPackageInstalls("fixture.one", 0),
                "Installer targeting pre-O Android acquired the O-only capability");
        installer.setTargetSdkVersion(android.os.Build.VERSION_CODES.O);
        require(service.canRequestPackageInstalls("fixture.one", 0),
                "Installer targeting Android O lost the capability at its API boundary");
        require(!service.canRequestPackageInstalls("fixture.two", 0)
                        && !service.canRequestPackageInstalls("fixture.absent", 0)
                        && !service.canRequestPackageInstalls(null, 0)
                        && !service.canRequestPackageInstalls("fixture.one", 10),
                "Package without permission, unknown package, null or secondary user could request installs");
        require(service.canRequestPackageInstalls("android", 0)
                        && !service.canRequestPackageInstalls("android", 10),
                "Platform package install-request status broke");
        removing.put("fixture.one", packages.get("fixture.one"));
        require(!service.canRequestPackageInstalls("fixture.one", 0),
                "Package pending removal could still request installs");
        removing.clear();

        require(service.getPackageGids("fixture.one", 0, 0).length == 0
                && service.getPackageGids("fixture.two", 0, 0).length == 0,
                "Known packages became unknown when querying supplementary groups");
        require(service.getPackageGids("android", 0, 0).length == 0,
                "Platform package lost its group inventory");
        require(service.getPackageGids("fixture.one", android.content.pm.PackageManager.MATCH_SYSTEM_ONLY, 0) == null,
                "System-only supplementary-group query included an ordinary package");
        require(service.getPackageGids("fixture.system", android.content.pm.PackageManager.MATCH_SYSTEM_ONLY, 0).length == 0
                        && service.getPackageGids("android", android.content.pm.PackageManager.MATCH_SYSTEM_ONLY, 0).length == 0,
                "System-only supplementary-group query excluded an addon or the platform");
        require(service.getPackageGids("fixture.absent", 0, 0) == null
                && service.getPackageGids("fixture.one", 0, 10) == null
                && service.getPackageGids("com.android.vending", 0, 0) == null,
                "Unknown package/user acquired groups");
        int[] uids = {12002, -1, 12001, 1000, 12001};
        String[] expectedNames = {"fixture.two", null, "fixture.one", "shared:android.uid.system", "fixture.one"};
        require(Arrays.equals(service.getNamesForUids(uids), expectedNames),
                "Bulk UID query lost request positions, duplicates or unknown names");
        String[] changed = service.getNamesForUids(uids);
        changed[0] = "caller.changed";
        require(Arrays.equals(service.getNamesForUids(uids), expectedNames),
                "Caller mutation changed the UID name inventory");
        require(service.getNamesForUids(new int[0]) == null && service.getNamesForUids(null) == null,
                "Absent UID input did not retain Android's null contract");
        packages.remove("fixture.one");
        require(service.getPackageGids("fixture.one", 0, 0) == null
                && Arrays.equals(service.getNamesForUids(new int[]{12001}), new String[]{null}),
                "Removed package remained in identity queries");

        List<String> calls = new ArrayList<>();
        IDexModuleRegisterCallback callback = new IDexModuleRegisterCallback.Default() {
            @Override public void onDexModuleRegistered(String path, boolean success, String message) {
                require(!Thread.holdsLock(service), "Callback ran with the package monitor held");
                require(!success && "registerDexModule call not supported since Android U".equals(message),
                        "Unsupported registration claimed success or lost its reason");
                calls.add(path);
                if ("first.dex".equals(path)) {
                    service.registerDexModule("fixture.two", "reentrant.dex", false, this);
                    require(calls.size() == 1, "Reentrant callback ran inline on the caller");
                }
            }
        };
        service.registerDexModule("fixture.one", "first.dex", false, callback);
        service.registerDexModule("fixture.two", "second.dex", true, callback);
        service.registerDexModule("fixture.one", "no-callback.dex", true, null);
        require(calls.isEmpty() && executor.work.size() == 2,
                "Registration failed to schedule exactly one asynchronous completion per callback");
        executor.runOne();
        require(calls.equals(Arrays.asList("first.dex")) && executor.work.size() == 2,
                "First completion lost identity or the queued reentrant request");
        executor.runOne();
        executor.runOne();
        require(calls.equals(Arrays.asList("first.dex", "second.dex", "reentrant.dex"))
                && executor.work.isEmpty(), "Callbacks were reordered, duplicated or omitted");

        int[] failedCalls = {0};
        service.registerDexModule("fixture.absent", "dead-client.dex", false,
                new IDexModuleRegisterCallback.Default() {
                    @Override public void onDexModuleRegistered(String path, boolean success, String message)
                            throws RemoteException {
                        failedCalls[0]++;
                        throw new RemoteException("fixture disconnected");
                    }
                });
        service.registerDexModule("fixture.two", "bad-local-client.dex", false,
                new IDexModuleRegisterCallback.Default() {
                    @Override public void onDexModuleRegistered(String path, boolean success, String message) {
                        failedCalls[0]++;
                        throw new IllegalStateException("fixture callback failed");
                    }
                });
        service.registerDexModule("fixture.two", "after-failure.dex", false, callback);
        executor.runOne();
        executor.runOne();
        executor.runOne();
        require(failedCalls[0] == 2 && calls.size() == 4 && "after-failure.dex".equals(calls.get(3))
                && executor.work.isEmpty(), "A failed callback was retried or blocked later completions");
        System.out.println("PackageServiceQueriesTest all passed: " + checks + " checks");
    }
}
