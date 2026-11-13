/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.app.ActivityManager;
import android.app.IApplicationThread;
import android.app.PendingIntent;
import android.content.ComponentName;
import android.content.Context;
import android.content.IIntentReceiver;
import android.content.IIntentSender;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.content.res.AssetManager;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;

import java.io.File;
import java.io.RandomAccessFile;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.Collection;
import java.util.List;

/** Actual controller authorization and PendingIntent replacement policy; no APK code runs. */
public final class DispatchSecurityTest {
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name); f.setAccessible(true); return f;
    }
    private static Method method(Class<?> type, String name, Class<?>... types) throws Exception {
        Method m = type.getDeclaredMethod(name, types); m.setAccessible(true); return m;
    }
    private static Object construct(Class<?> type, Class<?>[] types, Object... args) throws Exception {
        Constructor<?> c = type.getDeclaredConstructor(types); c.setAccessible(true); return c.newInstance(args);
    }
    private interface Operation { void run() throws Exception; }
    private static void denied(Operation action) throws Exception {
        try { action.run(); }
        catch (InvocationTargetException e) { if (e.getCause() instanceof SecurityException) return; throw e; }
        catch (SecurityException expected) { return; }
        throw new AssertionError("Unauthorized component operation accepted");
    }
    private static final class ThreadStub extends IApplicationThread.Default {
        final Binder binder = new Binder();
        int broadcasts;
        int manifestBroadcasts;
        @Override public IBinder asBinder() { return binder; }
        @Override public void scheduleRegisteredReceiver(IIntentReceiver receiver, Intent intent,
                int resultCode, String data, Bundle extras, boolean ordered, boolean sticky,
                boolean assumeDelivered, int sendingUser, int processState, int sendingUid,
                String sendingPackage) { broadcasts++; }
        @Override public void scheduleReceiver(Intent intent, ActivityInfo info,
                android.content.res.CompatibilityInfo compat, int resultCode, String data,
                Bundle extras, boolean ordered, boolean assumeDelivered, int sendingUser,
                int processState, int sendingUid, String sendingPackage) { manifestBroadcasts++; }
    }
    private static final class ReceiverStub extends IIntentReceiver.Default {
        final Binder binder = new Binder();
        @Override public IBinder asBinder() { return binder; }
    }

    @SuppressWarnings("unchecked")
    public static void main(String[] args) throws Exception {
        require(args.length == 2, "Pass signing-fixtures directory and official GmsCore APK");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        File data = Files.createTempDirectory("gesher-dispatch-security-").toFile();
        android.system.Os.setenv("ANDROID_DATA", data.getAbsolutePath(), true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true);
        Class<?> apk = Class.forName("com.gesher.server.ApkPackage");
        Method parse = apk.getMethod("parse", String.class);
        Object sender = parse.invoke(null, new File(args[0], "v2only.apk").getPath());
        Object owner = parse.invoke(null, args[1]);
        String senderPackage = (String) apk.getField("packageName").get(sender);
        String ownerPackage = (String) apk.getField("packageName").get(owner);
        Class<?> pmType = Class.forName("com.gesher.server.PackageManagerService");
        Class<?> atmType = Class.forName("com.gesher.server.ActivityTaskManagerService");
        Class<?> amType = Class.forName("com.gesher.server.ActivityManagerService");
        Class<?> notificationsType = Class.forName("com.gesher.server.NotificationManagerService");
        Object pm = construct(pmType, new Class<?>[] {Collection.class}, Arrays.asList(sender, owner));
        Object atm = construct(atmType, new Class<?>[] {pmType}, pm);
        Object am = construct(amType, new Class<?>[] {atmType, pmType, notificationsType}, atm, pm,
                construct(notificationsType, new Class<?>[0]));
        Object services = field(amType, "mServices").get(am);
        Object broadcasts = field(amType, "mBroadcasts").get(am);
        Class<?> servicesType = services.getClass(), broadcastType = broadcasts.getClass();
        PackageInfo info = (PackageInfo) apk.getMethod("packageInfo", long.class)
                .invoke(owner, (long) (PackageManager.GET_SERVICES | PackageManager.GET_RECEIVERS));
        ServiceInfo hidden = null, guarded = null;
        for (ServiceInfo service : info.services) {
            if (!service.exported && hidden == null) hidden = service;
            if (service.exported && service.permission != null && guarded == null) guarded = service;
        }
        require(hidden != null && guarded != null, "GmsCore lacks private/guarded service fixtures");
        final Intent hiddenService = new Intent().setComponent(new ComponentName(ownerPackage, hidden.name));
        final Intent guardedService = new Intent().setComponent(new ComponentName(ownerPackage, guarded.name));
        Method startService = method(servicesType, "startService", IApplicationThread.class,
                Intent.class, String.class, boolean.class, String.class);
        Method stopService = method(servicesType, "stopService", IApplicationThread.class, Intent.class, String.class);
        Method bindService = method(servicesType, "bindService", IApplicationThread.class,
                Intent.class, String.class, android.app.IServiceConnection.class, long.class, String.class);
        denied(() -> startService.invoke(services, null, hiddenService, null, false, senderPackage));
        denied(() -> startService.invoke(services, null, guardedService, null, false, senderPackage));
        android.app.IServiceConnection connection = new android.app.IServiceConnection.Default() {
            final Binder binder = new Binder(); @Override public IBinder asBinder() { return binder; }
        };
        denied(() -> bindService.invoke(services, null, hiddenService, null, connection,
                (long) Context.BIND_AUTO_CREATE, senderPackage));
        require(((java.util.Map<?, ?>) field(servicesType, "mServices").get(services)).isEmpty(),
                "Rejected service access created service state");
        System.out.println("DispatchSecurityTest: private and permission-guarded service start/bind denied");

        ActivityInfo hiddenReceiver = null;
        for (ActivityInfo receiver : info.receivers) if (!receiver.exported) { hiddenReceiver = receiver; break; }
        require(hiddenReceiver != null, "GmsCore lacks a private manifest receiver fixture");
        ThreadStub receiverThread = new ThreadStub();
        Class<?> processType = Class.forName(atmType.getName() + "$ProcessRecord");
        Object process = construct(processType, new Class<?>[] {atmType, int.class, IApplicationThread.class},
                atm, 888891, receiverThread);
        field(processType, "packageName").set(process, ownerPackage);
        field(processType, "processName").set(process, hiddenReceiver.processName);
        field(processType, "ready").setBoolean(process, true);
        ((List<Object>) field(atmType, "mProcesses").get(atm)).add(process);
        Method register = method(broadcastType, "registerReceiver", IApplicationThread.class, String.class,
                IIntentReceiver.class, IntentFilter.class, int.class, String.class, int.class);
        Method broadcast = method(broadcastType, "broadcastIntent", Intent.class, String.class,
                String.class, int.class, String[].class, String[].class, String[].class);
        ReceiverStub privateReceiver = new ReceiverStub();
        register.invoke(broadcasts, receiverThread, ownerPackage, privateReceiver,
                new IntentFilter("test.dispatch.PRIVATE"), 888891, null, Context.RECEIVER_NOT_EXPORTED);
        broadcast.invoke(broadcasts, new Intent("test.dispatch.PRIVATE"), null, senderPackage,
                android.os.Process.myPid(), null, null, null);
        require(receiverThread.broadcasts == 0, "Non-exported registered receiver got a cross-app broadcast");
        register.invoke(broadcasts, receiverThread, ownerPackage, new ReceiverStub(),
                new IntentFilter("test.dispatch.GUARDED"), 888891, "test.dispatch.OWNER_ONLY", Context.RECEIVER_EXPORTED);
        broadcast.invoke(broadcasts, new Intent("test.dispatch.GUARDED"), null, senderPackage,
                android.os.Process.myPid(), null, null, null);
        require(receiverThread.broadcasts == 0, "Receiver's sender permission was ignored");
        register.invoke(broadcasts, receiverThread, ownerPackage, new ReceiverStub(),
                new IntentFilter("test.dispatch.PUBLIC"), 888891, null, Context.RECEIVER_EXPORTED);
        broadcast.invoke(broadcasts, new Intent("test.dispatch.PUBLIC"), null, senderPackage,
                android.os.Process.myPid(), new String[]{"test.dispatch.UNGRANTED"}, null, null);
        require(receiverThread.broadcasts == 0, "Required recipient permission was ignored");
        broadcast.invoke(broadcasts, new Intent("test.dispatch.PUBLIC"), null, senderPackage,
                android.os.Process.myPid(), null, null, null);
        require(receiverThread.broadcasts == 1, "Allowed exported receiver did not receive its broadcast");
        Intent privateManifest = new Intent("test.dispatch.PRIVATE_MANIFEST")
                .setComponent(new ComponentName(ownerPackage, hiddenReceiver.name));
        broadcast.invoke(broadcasts, privateManifest, null, senderPackage,
                android.os.Process.myPid(), null, null, null);
        require(receiverThread.manifestBroadcasts == 0, "Private manifest receiver got a cross-app broadcast");
        broadcast.invoke(broadcasts, privateManifest, null, ownerPackage,
                android.os.Process.myPid(), null, null, null);
        require(receiverThread.manifestBroadcasts == 1, "Private receiver rejected its own package");
        final Intent forgedPush = new Intent("com.google.android.c2dm.intent.RECEIVE").setPackage(ownerPackage);
        denied(() -> broadcast.invoke(broadcasts, forgedPush, null, senderPackage,
                android.os.Process.myPid(), null, null, null));
        denied(() -> broadcast.invoke(broadcasts,
                new Intent(Intent.ACTION_BOOT_COMPLETED).setPackage(senderPackage), null,
                senderPackage, android.os.Process.myPid(), null, null, null));
        System.out.println("DispatchSecurityTest: receiver exports, sender/recipient permissions and forged FCM denied");

        Method get = amType.getMethod("getIntentSender", int.class, String.class, IBinder.class,
                String.class, int.class, Intent[].class, String[].class, int.class, Bundle.class, int.class);
        Intent pending = new Intent("test.dispatch.PENDING").setPackage(senderPackage).putExtra("marker", "original");
        IIntentSender capability = (IIntentSender) get.invoke(am, ActivityManager.INTENT_SENDER_BROADCAST,
                senderPackage, null, null, 731, new Intent[]{pending}, null,
                PendingIntent.FLAG_IMMUTABLE, null, 0);
        Object controller = field(amType, "mPendingIntents").get(am);
        Class<?> recordType = capability.getClass();
        Method same = method(recordType, "sameKey", int.class, String.class, int.class,
                String.class, int.class, Intent.class, String.class, int.class);
        int uid = field(recordType, "uid").getInt(capability);
        String identity = (String) field(recordType, "creatorIdentity").get(capability);
        require((Boolean) same.invoke(capability, ActivityManager.INTENT_SENDER_BROADCAST,
                senderPackage, uid, identity, 731, pending, null, PendingIntent.FLAG_IMMUTABLE), "Own PI key did not match");
        require(!(Boolean) same.invoke(capability, ActivityManager.INTENT_SENDER_BROADCAST,
                senderPackage, uid + 1, identity, 731, pending, null, PendingIntent.FLAG_IMMUTABLE), "PI key ignored UID");
        require(!(Boolean) same.invoke(capability, ActivityManager.INTENT_SENDER_BROADCAST,
                senderPackage, uid, identity, 731, pending, null, PendingIntent.FLAG_MUTABLE), "PI key ignored mutability");
        Object replacement = parse.invoke(null, new File(args[0], "rotated.apk").getPath());
        method(pmType, "add", apk).invoke(pm, replacement);
        Method send = method(controller.getClass(), "send", recordType, Intent.class, String.class);
        require((Integer) send.invoke(controller, capability, null, null) == ActivityManager.START_CANCELED,
                "Old signer PendingIntent remained usable after creator code replacement");
        Object unsigned = parse.invoke(null, new File(args[0], "unsigned.apk").getPath());
        method(pmType, "add", apk).invoke(pm, unsigned);
        IIntentSender unsignedCapability = (IIntentSender) get.invoke(am, ActivityManager.INTENT_SENDER_BROADCAST,
                senderPackage, null, null, 732, new Intent[]{pending}, null,
                PendingIntent.FLAG_IMMUTABLE, null, 0);
        File changedDirectory = new File(data, "changed-unsigned"); changedDirectory.mkdirs();
        File changedUnsigned = new File(changedDirectory, "unsigned.apk");
        Files.copy(new File(args[0], "unsigned.apk").toPath(), changedUnsigned.toPath());
        try (RandomAccessFile file = new RandomAccessFile(changedUnsigned, "rw")) {
            file.seek(10); int value = file.readUnsignedByte(); file.seek(10); file.writeByte(value ^ 1);
        }
        method(pmType, "add", apk).invoke(pm, parse.invoke(null, changedUnsigned.getPath()));
        require((Integer) send.invoke(controller, unsignedCapability, null, null) == ActivityManager.START_CANCELED,
                "Unsigned replacement retained a previous code version's capability");
        System.out.println("DispatchSecurityTest: PendingIntent UID/mutability keys and replaced-creator revocation passed");
        System.out.println("DispatchSecurityTest all passed");
    }
}
