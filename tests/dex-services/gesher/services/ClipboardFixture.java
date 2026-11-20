/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.app.IApplicationThread;
import android.content.ClipData;
import android.content.IClipboard;
import android.content.IOnPrimaryClipChangedListener;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PermissionInfo;
import android.content.pm.Signature;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteCallbackList;
import android.os.RemoteException;
import android.provider.Settings;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedAttributionImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.AppOpsService;
import com.gesher.server.ClipboardService;
import com.gesher.server.PackageManagerService;
import com.gesher.server.StandIn;
import com.gesher.server.WindowsHost;
import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicInteger;
import sun.misc.Unsafe;

/** Actual PM/ATM/AppOps/Settings/Clipboard services; only APK metadata and native observations are fixtures. */
public final class ClipboardFixture implements AutoCloseable {
    static final int UID = 12767, PID = 887670;
    static final String OWNER = "fixture.clipboard.owner", PEER = "fixture.clipboard.peer";
    static final String PRIVILEGED = "fixture.clipboard.privileged", IME = "fixture.clipboard.ime";
    static final String HOST_PACKAGE = "com.gesher.app";
    static final long CREATED = 133700000000000000L;
    interface Checked { void run() throws Exception; }
    static final Unsafe UNSAFE;
    static {
        try { Field field = Unsafe.class.getDeclaredField("theUnsafe"); field.setAccessible(true); UNSAFE = (Unsafe) field.get(null); }
        catch (Exception failure) { throw new ExceptionInInitializerError(failure); }
    }
    static Class<?> type(String name) throws Exception { return Class.forName("com.gesher.server." + name); }
    static Field field(Class<?> type, String name) throws Exception { Field field = type.getDeclaredField(name); field.setAccessible(true); return field; }
    static Object get(Object target, String name) throws Exception { return field(target.getClass(), name).get(target); }
    static void set(Object target, String name, Object value) throws Exception { field(target.getClass(), name).set(target, value); }
    static Object make(Class<?> type, Class<?>[] signature, Object... values) throws Exception {
        Constructor<?> constructor = type.getDeclaredConstructor(signature); constructor.setAccessible(true); return constructor.newInstance(values);
    }
    static Object call(Object target, String name, Class<?>[] signature, Object... values) throws Exception {
        Method method = target.getClass().getDeclaredMethod(name, signature); method.setAccessible(true); return method.invoke(target, values);
    }
    static ApkPackage pkg(String name, int uid, byte certificate, String... permissions) throws Exception {
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(36);
        for (String permission : permissions) parsed.addUsesPermission(new ParsedUsesPermissionImpl(permission, 0, java.util.Set.of()));
        parsed.addAttribution(new ParsedAttributionImpl("clipboard", 1, List.of()));
        parsed.hideAsFinal();
        ApkPackage code = (ApkPackage) UNSAFE.allocateInstance(ApkPackage.class);
        set(code, "packageName", name); set(code, "parsed", parsed); set(code, "mUid", uid);
        set(code, "apkPath", "/fixture/" + name + "/base.apk"); set(code, "mDataDir", "/fixture/" + name + "/data");
        set(code, "sharedLibraries", field(type("SharedLibraryGraph"), "EMPTY").get(null));
        set(code, "mSigningIdentityChecked", true);
        set(code, "mVerifiedSignatures", new Signature[]{new Signature(new byte[]{certificate})});
        return code;
    }
    static IBinder parcelTransport(IBinder target) {
        return new Binder() {
            @Override public IInterface queryLocalInterface(String descriptor) { return null; }
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
                return target.transact(code, data, reply, flags);
            }
        };
    }
    static final class Token extends Binder {
        final List<IBinder.DeathRecipient> deaths = new ArrayList<>();
        @Override public synchronized void linkToDeath(IBinder.DeathRecipient recipient, int flags) { deaths.add(recipient); }
        @Override public synchronized boolean unlinkToDeath(IBinder.DeathRecipient recipient, int flags) { return deaths.remove(recipient); }
        void die() {
            List<IBinder.DeathRecipient> snapshot;
            synchronized (this) { snapshot = new ArrayList<>(deaths); deaths.clear(); }
            for (IBinder.DeathRecipient recipient : snapshot) recipient.binderDied();
        }
    }
    static final class Listener extends IOnPrimaryClipChangedListener.Default {
        final Token token = new Token();
        final AtomicInteger calls = new AtomicInteger();
        Runnable action;
        @Override public IBinder asBinder() { return token; }
        @Override public void dispatchPrimaryClipChanged() { calls.incrementAndGet(); if (action != null) action.run(); }
    }
    static final class NativeState {
        volatile long[] foreground = new long[]{PID, CREATED};
        volatile int lockState;
        volatile int sequence = 1;
        volatile String text, html;
        volatile byte[] image;
        volatile int writes, clears;
        volatile boolean changeAfterWrite;
        final CopyOnWriteArrayList<WindowsHost.Listener> listeners = new CopyOnWriteArrayList<>();
        Object boundary() throws Exception {
            Class<?> host = type("ClipboardService$Host");
            return Proxy.newProxyInstance(host.getClassLoader(), new Class<?>[]{host}, (proxy, method, args) -> {
                switch (method.getName()) {
                    case "available": return true;
                    case "foregroundProcess": return foreground == null ? null : foreground.clone();
                    case "sessionLockState": return lockState;
                    case "sequence": return sequence;
                    case "text": return text;
                    case "html": return html;
                    case "image": return image;
                    case "write":
                        text = (String)args[0]; html = (String)args[1]; image = (byte[])args[2]; sequence++; writes++;
                        int written = sequence;
                        if (changeAfterWrite) { text = "external-after-write"; html = null; image = null; sequence++; }
                        return written;
                    case "clear": text = null; html = null; image = null; sequence++; clears++; return sequence;
                    case "addListener": listeners.add((WindowsHost.Listener)args[0]); return null;
                    case "removeListener": listeners.remove(args[0]); return null;
                    case "toString": return "isolated clipboard native observations";
                    case "hashCode": return System.identityHashCode(proxy);
                    case "equals": return proxy == args[0];
                    default: throw new AssertionError(method.getName());
                }
            });
        }
        void external(String value, byte[] bytes) {
            text = value; html = null; image = bytes; sequence++;
            for (WindowsHost.Listener listener : listeners) listener.onWindowsEvents(WindowsHost.EVENT_CLIPBOARD);
        }
    }

    final ApkPackage owner, peer, privileged, ime, hostPackage;
    final PackageManagerService pm;
    final Object atm, settings, previousSettings;
    final List<Object> processes = new ArrayList<>();
    final AppOpsService ops;
    final NativeState nativeState;
    final ClipboardService service;
    final IClipboard api;
    final File settingsFile;

    @SuppressWarnings("unchecked") ClipboardFixture(boolean binder, boolean realHost) throws Exception {
        owner = pkg(OWNER, UID, (byte)7, Manifest.permission.READ_CLIPBOARD_IN_BACKGROUND, Manifest.permission.SET_CLIP_SOURCE);
        peer = pkg(PEER, UID + 1, (byte)8);
        privileged = pkg(PRIVILEGED, UID + 2, (byte)42, Manifest.permission.READ_CLIPBOARD_IN_BACKGROUND, Manifest.permission.SET_CLIP_SOURCE);
        ime = pkg(IME, UID + 3, (byte)9);
        hostPackage = pkg(HOST_PACKAGE, UID + 4, (byte)10);
        ApkPackage platform = pkg("android", android.os.Process.SYSTEM_UID, (byte)42);
        pm = (PackageManagerService)make(PackageManagerService.class, new Class<?>[]{Collection.class, boolean.class},
                List.of(owner, peer, privileged, ime, hostPackage, platform), false);
        set(pm, "mPackageEventSender", (java.util.function.Consumer<Intent>)event -> { });
        atm = make(type("ActivityTaskManagerService"), new Class<?>[]{PackageManagerService.class}, pm);
        make(type("ActivityManagerService"), new Class<?>[]{atm.getClass(), PackageManagerService.class, type("NotificationManagerService")}, atm, pm, null);
        if (!binder) {
            // Cached host classes have no framework-res.apk; only supply the Android manifest definitions.
            Object catalog = get(pm.permissions(), "mCatalog");
            Map<String, PermissionInfo> definitions = (Map<String, PermissionInfo>)get(catalog, "mFramework");
            for (String name : List.of(Manifest.permission.READ_CLIPBOARD_IN_BACKGROUND, Manifest.permission.SET_CLIP_SOURCE)) {
                PermissionInfo info = new PermissionInfo(); info.name = name; info.packageName = "android";
                info.protectionLevel = PermissionInfo.PROTECTION_SIGNATURE | (name.equals(Manifest.permission.READ_CLIPBOARD_IN_BACKGROUND)
                        ? PermissionInfo.PROTECTION_FLAG_ROLE : PermissionInfo.PROTECTION_FLAG_RECENTS);
                definitions.put(name, info);
            }
            set(catalog, "mLoaded", true);
        }
        ops = pm.appOps();
        int index = 0;
        for (ApkPackage code : List.of(owner, peer, privileged, ime, hostPackage)) {
            final int ordinal = index++;
            as(code, () -> {
                IApplicationThread thread = new IApplicationThread.Default() {
                    final Binder token = new Binder(); @Override public IBinder asBinder() { return token; }
                };
                Object process = make(type("ActivityTaskManagerService$ProcessRecord"),
                        new Class<?>[]{atm.getClass(), int.class, IApplicationThread.class}, atm, PID + ordinal, thread);
                set(process, "packageName", code.packageName); set(process, "processName", code.packageName);
                set(process, "packageCode", code); set(process, "ready", true); set(process, "nativeStartTime", CREATED + ordinal);
                ((List<Object>)get(atm, "mProcesses")).add(process); processes.add(process);
            });
        }
        previousSettings = field(type("SettingsProvider"), "sInstance").get(null);
        settingsFile = File.createTempFile("clipboard-settings-", ".bin"); settingsFile.delete();
        settings = make(type("SettingsProvider"), new Class<?>[]{PackageManagerService.class, atm.getClass(), File.class}, pm, atm, settingsFile);
        nativeState = realHost ? null : new NativeState();
        service = realHost ? (ClipboardService)make(ClipboardService.class,
                new Class<?>[]{atm.getClass(), PackageManagerService.class}, atm, pm)
                : (ClipboardService)make(ClipboardService.class,
                    new Class<?>[]{atm.getClass(), PackageManagerService.class, type("ClipboardService$Host")}, atm, pm, nativeState.boundary());
        api = binder ? IClipboard.Stub.asInterface(parcelTransport(StandIn.of(IClipboard.class, service).asBinder())) : service;
    }
    ClipboardFixture(boolean binder) throws Exception { this(binder, false); }
    void as(ApkPackage code, Checked action) throws Exception {
        long identity = Binder.clearCallingIdentity();
        try { Binder.restoreCallingIdentity(((long)code.uid() << 32) | (PID + code.uid() - UID)); action.run(); }
        finally { Binder.restoreCallingIdentity(identity); }
    }
    void asOwner(Checked action) throws Exception { as(owner, action); }
    @SuppressWarnings({"unchecked", "rawtypes"}) void resumed(boolean visible) throws Exception {
        Object process = processes.get(0);
        List<Object> activities = (List<Object>)get(process, "activities"); activities.clear();
        set(atm, "mTopResumed", null);
        if (!visible) return;
        ActivityInfo info = new ActivityInfo(); info.packageName = OWNER; info.name = OWNER + ".Activity";
        Object activity = make(type("ActivityTaskManagerService$ActivityRecord"),
                new Class<?>[]{atm.getClass(), ActivityInfo.class, Intent.class, int.class, int.class, String.class,
                    type("ActivityTaskManagerService$ActivityRecord"), String.class, int.class},
                atm, info, new Intent(), 1, PID, OWNER, null, null, 0);
        set(activity, "app", process);
        set(activity, "state", Enum.valueOf((Class)type("ActivityTaskManagerService$State"), "RESUMED"));
        activities.add(activity); set(atm, "mTopResumed", activity);
    }
    void write(String value) throws Exception {
        asOwner(() -> api.setPrimaryClip(ClipData.newPlainText("fixture", value), OWNER, "clipboard", 0, 0));
    }
    void ime(String value) throws Exception {
        Bundle values = new Bundle(); values.putString("value", value);
        call(settings, "call", new Class<?>[]{String.class, String.class, Bundle.class}, "PUT_secure", Settings.Secure.DEFAULT_INPUT_METHOD, values);
    }
    void add(ApkPackage code, Listener listener) throws Exception {
        // Local callback/death fixtures use the production list directly. The Windows test
        // separately traverses the generated Binder clipboard proxy for its read/write calls.
        as(code, () -> service.addPrimaryClipChangedListener(listener, code.packageName, "clipboard", 0, 0));
    }
    void remove(ApkPackage code, Listener listener) throws Exception {
        as(code, () -> service.removePrimaryClipChangedListener(listener, code.packageName, "clipboard", 0, 0));
    }
    RemoteCallbackList<?> listeners() throws Exception { return (RemoteCallbackList<?>)get(service, "mListeners"); }
    @Override public void close() throws Exception {
        service.close();
        field(type("SettingsProvider"), "sInstance").set(null, previousSettings);
        settingsFile.delete();
    }
}
