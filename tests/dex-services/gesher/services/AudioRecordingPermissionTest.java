/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.app.ActivityThread;
import android.content.AttributionSource;
import android.content.ContextWrapper;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.content.pm.Signature;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.Process;
import android.os.RemoteException;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.AudioService;
import com.gesher.server.PackageManagerService;
import com.gesher.server.PackageUids;
import com.gesher.server.StandIn;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.Set;
import sun.misc.Unsafe;

/** Real audio admission and UID permission store; installed APK metadata is a fixture. */
public final class AudioRecordingPermissionTest {
    private static final String OWNER = "fixture.recording.owner";
    private static final String SIBLING = "fixture.recording.sibling";
    private static final String PEER = "fixture.recording.peer";
    private static final String RECORD = Manifest.permission.RECORD_AUDIO;
    private static final Unsafe UNSAFE;
    private static int checks;
    private static boolean nativeBridge, binder;
    static {
        try {
            Field f = Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true);
            UNSAFE = (Unsafe)f.get(null);
        } catch (ReflectiveOperationException error) { throw new ExceptionInInitializerError(error); }
    }
    /** Host only: the exact production MayRecordAudio JNI body calls the real Java policy. */
    public static native boolean nativeAllowed(boolean throughProvider);
    private interface Work { void run() throws Exception; }
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name); f.setAccessible(true); return f;
    }
    private static void set(Object target, String name, Object value) throws Exception {
        field(target.getClass(), name).set(target, value);
    }
    private static Object call(Object target, String name, Class<?>[] signature, Object... args)
            throws Exception {
        var method = target.getClass().getDeclaredMethod(name, signature); method.setAccessible(true);
        try { return method.invoke(target, args); }
        catch (InvocationTargetException error) {
            if (error.getCause() instanceof Exception cause) throw cause;
            throw new AssertionError("Fixture publication failed", error.getCause());
        }
    }
    private static void denied(Class<? extends Throwable> type, Work action, String message)
            throws Exception {
        try { action.run(); }
        catch (Throwable error) {
            if (type.isInstance(error)) { checks++; return; }
            throw new AssertionError(message + ": wrong exception " + error, error);
        }
        throw new AssertionError(message + ": operation was accepted");
    }
    private static ApkPackage code(String name, int uid, boolean requested) throws Exception {
        String path = "/fixture/recording/" + name + "/base.apk";
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, path);
        parsed.setEnabled(true); parsed.setTargetSdkVersion(36);
        parsed.setSharedUserId("fixture.recording.shared." + uid);
        if (requested) parsed.addUsesPermission(new ParsedUsesPermissionImpl(RECORD, 0, Set.of()));
        parsed.hideAsFinal();
        ApkPackage apk = (ApkPackage)UNSAFE.allocateInstance(ApkPackage.class);
        set(apk, "packageName", name); set(apk, "parsed", parsed); set(apk, "mUid", uid);
        set(apk, "apkPath", path); set(apk, "mDataDir", "/fixture/recording/" + name + "/data");
        set(apk, "sharedLibraries", field(Class.forName("com.gesher.server.SharedLibraryGraph"), "EMPTY").get(null));
        set(apk, "mSigningIdentityChecked", true);
        set(apk, "mVerifiedSignatures", new Signature[]{new Signature(new byte[]{4, 7, 1})});
        return apk;
    }
    private static IBinder remote(IBinder target) {
        return new Binder() {
            @Override public IInterface queryLocalInterface(String name) { return null; }
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                    throws RemoteException { return target.transact(code, data, reply, flags); }
        };
    }
    private static void manager(IPackageManager manager) throws Exception {
        field(ActivityThread.class, "sPackageManager").set(null, manager);
    }
    private static void metadata(String name, int claimedUid) throws Exception {
        if (name == null) {
            field(ActivityThread.class, "sCurrentActivityThread").set(null, null); return;
        }
        Object thread = UNSAFE.allocateInstance(ActivityThread.class);
        Object bound = UNSAFE.allocateInstance(Class.forName("android.app.ActivityThread$AppBindData"));
        ApplicationInfo info = new ApplicationInfo(); info.packageName = name; info.uid = claimedUid;
        info.targetSdkVersion = 36;
        set(bound, "appInfo", info); set(thread, "mBoundApplication", bound);
        field(ActivityThread.class, "sCurrentActivityThread").set(null, thread);
    }
    private static void admission(boolean expected, String message) {
        check(AudioService.recordAudioAllowed() == expected, message + " (Java)");
        if (nativeBridge) {
            check(nativeAllowed(false) == expected, message + " (actual JNI body)");
            check(nativeAllowed(true) == expected, message + " (actual native input provider)");
        }
    }
    private static ContextWrapper context() {
        return new ContextWrapper(null) {
            @Override public String getPackageName() { return OWNER; }
            @Override public ApplicationInfo getApplicationInfo() {
                ApplicationInfo info = new ApplicationInfo(); info.packageName = OWNER;
                info.uid = Process.myUid(); info.targetSdkVersion = 36; return info;
            }
            @Override public AttributionSource getAttributionSource() {
                return new AttributionSource.Builder(Process.myUid()).setPackageName(OWNER).build();
            }
            @Override public int getDeviceId() { return 0; }
        };
    }
    private static void deniedPublicAdapters() throws Exception {
        if (!binder) return;
        AudioRecord record = new AudioRecord(MediaRecorder.AudioSource.MIC, 44100,
                AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, 4096);
        try {
            check(record.getState() == AudioRecord.STATE_UNINITIALIZED,
                    "Denied AudioRecord constructor preserves STATE_UNINITIALIZED");
            denied(IllegalStateException.class, record::startRecording,
                    "Denied AudioRecord cannot start");
        } finally { record.release(); }
        denied(UnsupportedOperationException.class, () -> new AudioRecord.Builder()
                .setContext(context()).setAudioSource(MediaRecorder.AudioSource.MIC)
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(44100)
                        .setChannelMask(AudioFormat.CHANNEL_IN_MONO)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                .setBufferSizeInBytes(4096).build(), "Denied AudioRecord Builder");
        MediaRecorder recorder = new MediaRecorder(context());
        try {
            denied(RuntimeException.class, () -> recorder.setAudioSource(MediaRecorder.AudioSource.MIC),
                    "Denied MediaRecorder source retains native error translation");
        } finally { recorder.release(); }
    }
    @SuppressWarnings("unchecked")
    public static void run(boolean withNativeBridge, boolean withBinder) throws Exception {
        nativeBridge = withNativeBridge; binder = withBinder; checks = 0;
        int uid = Process.myUid();
        check(uid >= Process.FIRST_APPLICATION_UID, "Fixture must run as an ordinary application UID");
        if (!binder) {
            // The host adapter fixes Process.myUid() at the first app UID.
            // Reserve both fixture principals before registering hosted packages.
            check(PackageUids.uidFor(OWNER) == uid && PackageUids.uidFor(PEER) == uid + 1,
                    "Host fixture package identities match the supplied process UIDs");
        }
        Field pmField = field(ActivityThread.class, "sPackageManager"); Object oldPm = pmField.get(null);
        Field threadField = field(ActivityThread.class, "sCurrentActivityThread"); Object oldThread = threadField.get(null);
        Field servicesField = field(Class.forName("com.gesher.server.Services"), "sLast");
        Object oldServices = servicesField.get(null); servicesField.set(null, null);
        try {
            ApkPackage owner = code(OWNER, uid, true), sibling = code(SIBLING, uid, true), peer = code(PEER, uid + 1, true);
            Constructor<PackageManagerService> constructor = PackageManagerService.class
                    .getDeclaredConstructor(Collection.class, boolean.class);
            constructor.setAccessible(true);
            PackageManagerService pm = constructor.newInstance(List.of(owner, sibling, peer), false);
            set(pm, "mPackageEventSender", (java.util.function.Consumer<android.content.Intent>) event -> {});
            if (!binder) {
                Object catalog = field(pm.permissions().getClass(), "mCatalog").get(pm.permissions());
                Map<String, PermissionInfo> definitions = (Map<String, PermissionInfo>)field(catalog.getClass(), "mFramework").get(catalog);
                PermissionInfo info = new PermissionInfo(); info.name = RECORD; info.packageName = "android";
                info.protectionLevel = PermissionInfo.PROTECTION_DANGEROUS;
                definitions.put(RECORD, info); set(catalog, "mLoaded", true);
                call(pm.permissions(), "initializePackages", new Class<?>[0]);
            }
            IPackageManager real = binder ? IPackageManager.Stub.asInterface(
                    remote(StandIn.of(IPackageManager.class, pm).asBinder())) : pm;
            manager(real); metadata(OWNER, uid);
            pm.permissions().revokeRuntimePermission(OWNER, RECORD, "default:0", 0, "fresh fixture");
            check(pm.checkUidPermission(RECORD, uid) == PackageManager.PERMISSION_DENIED,
                    "Real policy denies an ungranted modern manifest request");
            admission(false, "Manifest declaration alone cannot authorize recording");
            deniedPublicAdapters();

            pm.permissions().grantRuntimePermission(PEER, RECORD, "default:0", 0);
            metadata(PEER, uid + 1);
            admission(false, "A foreign current-package name and appInfo UID cannot lend its grant");
            metadata(null, uid);
            admission(false, "Missing ActivityThread metadata is not a grant");
            metadata(OWNER, uid);
            long previous = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long)Process.SYSTEM_UID << 32) | 881122);
                admission(false, "An incoming system Binder identity cannot lend its grant");
                check(Binder.getCallingUid() == Process.SYSTEM_UID && Binder.getCallingPid() == 881122,
                        "Admission restores incoming Binder identity");
            } finally { Binder.restoreCallingIdentity(previous); }

            pm.permissions().grantRuntimePermission(OWNER, RECORD, "default:0", 0);
            admission(true, "A live runtime grant permits new recording admission");
            metadata(null, uid);
            admission(true, "A native caller without package metadata resolves through its real UID");
            metadata(OWNER, uid);
            if (binder) {
                MediaRecorder recorder = new MediaRecorder(context());
                try { recorder.setAudioSource(MediaRecorder.AudioSource.MIC); checks++; }
                finally { recorder.release(); }
            }
            pm.permissions().revokeRuntimePermission(OWNER, RECORD, "default:0", 0, "fresh admission after revoke");
            admission(false, "Revocation is observed without a cached allow result");
            deniedPublicAdapters();

            pm.permissions().grantRuntimePermission(SIBLING, RECORD, "default:0", 0);
            call(pm, "add", new Class<?>[]{ApkPackage.class}, code(OWNER, uid, false));
            admission(true, "A legitimate shared-UID sibling supplies the runtime grant");
            call(pm, "remove", new Class<?>[]{String.class}, SIBLING);
            admission(false, "Removing the last requesting UID member removes admission");
            call(pm, "remove", new Class<?>[]{String.class}, OWNER);
            metadata(null, uid);
            admission(false, "Unknown application UID fails closed despite a permitted foreign UID");

            manager(new IPackageManager.Default() {
                @Override public int checkUidPermission(String permission, int requestedUid) throws RemoteException {
                    check(RECORD.equals(permission) && requestedUid == uid, "Resolver receives actual UID and permission");
                    check(Binder.getCallingPid() == Process.myPid(), "Resolver does not inherit incidental Binder caller");
                    throw new RemoteException("isolated permission resolver failure");
                }
            });
            previous = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long)(uid + 1) << 32) | 881123);
                admission(false, "Remote permission resolver failure denies");
                check(Binder.getCallingPid() == 881123 && Binder.getCallingUid() == uid + 1,
                        "Failed resolver restores incoming identity");
            } finally { Binder.restoreCallingIdentity(previous); }
            manager(new IPackageManager.Default() {
                @Override public int checkUidPermission(String permission, int requestedUid) {
                    throw new SecurityException("isolated rejected resolver");
                }
            });
            admission(false, "Rejected permission resolver denies");
            manager(new IPackageManager.Default() {
                @Override public int checkUidPermission(String permission, int requestedUid) { return 7; }
            });
            admission(false, "Only the exact granted result authorizes recording");
            check(Boolean.TRUE.equals(call(field(pm.permissions().getClass(), "mWriter").get(pm.permissions()),
                    "flush", new Class<?>[0])), "Fixture permission decisions finish their real durable write");
        } finally {
            pmField.set(null, oldPm); threadField.set(null, oldThread); servicesField.set(null, oldServices);
        }
        System.out.println("AudioRecordingPermissionTest all passed: " + checks
                + " checks; binder=" + binder + "; production-jni=" + nativeBridge);
    }
    public static void main(String[] args) throws Exception { run(false, true); }
}
