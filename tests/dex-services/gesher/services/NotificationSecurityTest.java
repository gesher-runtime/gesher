/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.app.Notification;
import android.app.ActivityManager;
import android.app.IApplicationThread;
import android.app.PendingIntent;
import android.content.ComponentName;
import android.content.Context;
import android.content.IIntentReceiver;
import android.content.IIntentSender;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.content.pm.ParceledListSlice;
import android.content.res.AssetManager;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.service.notification.INotificationListener;
import android.service.notification.StatusBarNotification;

import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.nio.file.Files;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.ToIntBiFunction;

/** Actual notification policy/storage/listener tests; the injected sink never shows Windows UI. */
public final class NotificationSecurityTest {
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    private interface Operation { void run() throws Exception; }
    private static void denied(Operation operation) throws Exception {
        try { operation.run(); }
        catch (SecurityException expected) { return; }
        catch (InvocationTargetException wrapped) {
            if (wrapped.getCause() instanceof SecurityException) return;
            throw wrapped;
        }
        throw new AssertionError("Unauthorized notification access accepted");
    }
    private static Method method(Class<?> type, String name, Class<?>... parameters) throws Exception {
        Method method = type.getDeclaredMethod(name, parameters); method.setAccessible(true); return method;
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field field = type.getDeclaredField(name); field.setAccessible(true); return field;
    }
    private static Object construct(Class<?> type, Class<?>[] types, Object... values) throws Exception {
        Constructor<?> constructor = type.getDeclaredConstructor(types);
        constructor.setAccessible(true); return constructor.newInstance(values);
    }
    private static Object service(Class<?> type, Class<?> pmType, Object pm,
            AtomicInteger shown) throws Exception {
        Constructor<?> constructor = type.getDeclaredConstructor(pmType,
                Class.forName("com.gesher.server.ActivityManagerService"), ToIntBiFunction.class);
        constructor.setAccessible(true);
        ToIntBiFunction<StatusBarNotification, Boolean> sink = (n, update) -> { shown.incrementAndGet(); return 0; };
        return constructor.newInstance(pm, null, sink);
    }
    private static File changedComment(File original, File directory) throws Exception {
        byte[] bytes = Files.readAllBytes(original.toPath());
        int eocd = -1;
        for (int i = bytes.length - 22; i >= Math.max(0, bytes.length - 65557); i--) {
            if ((bytes[i]&255)==0x50 && (bytes[i+1]&255)==0x4b
                    && (bytes[i+2]&255)==5 && bytes[i+3]==6) {
                int size = (bytes[i+20]&255) | ((bytes[i+21]&255)<<8);
                if (i + 22 + size == bytes.length && size < 65535) { eocd = i; break; }
            }
        }
        require(eocd >= 0, "ZIP fixture has no valid end record");
        byte[] changed = java.util.Arrays.copyOf(bytes, bytes.length+1);
        int size = ((bytes[eocd+20]&255) | ((bytes[eocd+21]&255)<<8)) + 1;
        changed[eocd+20]=(byte)size; changed[eocd+21]=(byte)(size>>>8); changed[changed.length-1]='x';
        directory.mkdirs(); File copy = new File(directory, original.getName());
        Files.write(copy.toPath(), changed); return copy;
    }

    /** The real broadcast controller dispatches here, where an attached app receives its callback. */
    private static final class ClickThread extends IApplicationThread.Default {
        final Binder binder = new Binder();
        int deliveries;
        Intent delivered;
        int senderUid;
        String senderPackage;
        @Override public IBinder asBinder() { return binder; }
        @Override public void scheduleRegisteredReceiver(IIntentReceiver receiver, Intent intent,
                int resultCode, String data, Bundle extras, boolean ordered, boolean sticky,
                boolean assumeDelivered, int sendingUser, int processState, int sendingUid,
                String sendingPackage) {
            deliveries++;
            delivered = new Intent(intent);
            senderUid = sendingUid;
            senderPackage = sendingPackage;
        }
    }
    private static final class ClickReceiver extends IIntentReceiver.Default {
        final Binder binder = new Binder();
        @Override public IBinder asBinder() { return binder; }
    }

    @SuppressWarnings("unchecked")
    private static void storedClickRestart(String probePath, String devPath, File data,
            Class<?> apk, Method parse, Class<?> pmType, Class<?> notifications,
            AtomicInteger shown) throws Exception {
        String pkg = "com.gesher.microgprobe", action = "test.notification.RESTORED_CLICK";
        Class<?> atmType = Class.forName("com.gesher.server.ActivityTaskManagerService");
        Class<?> amType = Class.forName("com.gesher.server.ActivityManagerService");
        Class<?> store = Class.forName("com.gesher.server.NotificationStore");
        Class<?> services = Class.forName("com.gesher.server.Services");
        Class<?> created = Class.forName("com.gesher.server.Services$Created");
        Field last = field(services, "sLast");
        Object prior = last.get(null);
        try {
            Object owner = parse.invoke(null, probePath);
            Object pm = pmType.getConstructor(java.util.Collection.class)
                    .newInstance(List.of(owner, parse.invoke(null, devPath)));
            Object atm = construct(atmType, new Class<?>[]{pmType}, pm);
            Object before = service(notifications, pmType, pm, shown);
            Object am = construct(amType, new Class<?>[]{atmType, pmType, notifications}, atm, pm, before);
            last.set(null, construct(created, new Class<?>[]{pmType, atmType, amType}, pm, atm, am));
            Method get = amType.getMethod("getIntentSender", int.class, String.class, IBinder.class,
                    String.class, int.class, Intent[].class, String[].class, int.class, Bundle.class, int.class);
            Intent content = new Intent(action).setPackage(pkg).putExtra("marker", "saved-content");
            Intent button = new Intent(action).setPackage(pkg).putExtra("marker", "saved-action");
            IIntentSender contentSender = (IIntentSender)get.invoke(am, ActivityManager.INTENT_SENDER_BROADCAST,
                    pkg, null, null, 75413, new Intent[]{content}, null, PendingIntent.FLAG_IMMUTABLE, null, 0);
            IIntentSender actionSender = (IIntentSender)get.invoke(am, ActivityManager.INTENT_SENDER_BROADCAST,
                    pkg, null, null, 75414, new Intent[]{button}, null, PendingIntent.FLAG_IMMUTABLE, null, 0);
            Notification saved = new Notification();
            saved.contentIntent = new PendingIntent(contentSender);
            saved.actions = new Notification.Action[]{new Notification.Action(0, "Restore", new PendingIntent(actionSender))};
            // Avoid AUTO_CANCEL: exercising an offline click must never call Windows toast APIs.
            String key = "0|"+pkg+"|75413|null|"+apk.getMethod("uid").invoke(owner);
            Method save = method(store, "save", amType, String.class, String.class, String.class,
                    int.class, Notification.class);
            Method load = method(store, "load", String.class, String.class);
            save.invoke(null, am, key, pkg, null, 75413, saved);
            require(load.invoke(null, pkg, key) != null, "Notification with real PendingIntents did not persist");

            // A service restart has neither the original notification nor its Binder capabilities.
            // Reparse the unchanged installed code and create fresh production controllers.
            Object reopenedOwner = parse.invoke(null, probePath);
            Object reopenedPm = pmType.getConstructor(java.util.Collection.class)
                    .newInstance(List.of(reopenedOwner, parse.invoke(null, devPath)));
            Object reopenedAtm = construct(atmType, new Class<?>[]{pmType}, reopenedPm);
            Object reopened = service(notifications, pmType, reopenedPm, shown);
            Object reopenedAm = construct(amType, new Class<?>[]{atmType, pmType, notifications},
                    reopenedAtm, reopenedPm, reopened);
            last.set(null, construct(created, new Class<?>[]{pmType, atmType, amType},
                    reopenedPm, reopenedAtm, reopenedAm));
            Object pending = field(amType, "mPendingIntents").get(reopenedAm);
            require(((List<?>)field(pending.getClass(), "mRecords").get(pending)).isEmpty(),
                    "Restart retained original in-memory PendingIntent capabilities");
            require(((java.util.Map<?,?>)field(notifications, "mPosted").get(reopened)).isEmpty(),
                    "Restart retained the original in-memory notification");
            ClickThread thread = new ClickThread();
            Class<?> processType = Class.forName(atmType.getName()+"$ProcessRecord");
            Object process = construct(processType, new Class<?>[]{atmType, int.class, IApplicationThread.class},
                    reopenedAtm, 888896, thread);
            field(processType, "packageName").set(process, pkg);
            field(processType, "processName").set(process, pkg);
            field(processType, "ready").setBoolean(process, true);
            ((List<Object>)field(atmType, "mProcesses").get(reopenedAtm)).add(process);
            Object broadcasts = field(amType, "mBroadcasts").get(reopenedAm);
            method(broadcasts.getClass(), "registerReceiver", IApplicationThread.class, String.class,
                    IIntentReceiver.class, IntentFilter.class, int.class, String.class, int.class)
                    .invoke(broadcasts, thread, pkg, new ClickReceiver(), new IntentFilter(action),
                            888896, null, Context.RECEIVER_NOT_EXPORTED);
            Object restored = load.invoke(null, pkg, key);
            require(restored != null, "Same installed code could not reload its notification after restart");
            Object restoredContent = field(restored.getClass(), "content").get(restored);
            require(restoredContent != null, "Content PendingIntent was lost from the stored notification");
            Method click = method(notifications, "activated", String.class);
            click.invoke(reopened, "n|"+key);
            require(thread.deliveries == 1 && "saved-content".equals(thread.delivered.getStringExtra("marker")),
                    "Restored content click did not dispatch the persisted Intent");
            require(thread.senderUid == (Integer)apk.getMethod("uid").invoke(reopenedOwner)
                            && pkg.equals(thread.senderPackage),
                    "Restored click lost its installed creator UID/package attribution");
            click.invoke(reopened, "a|0|"+key);
            require(thread.deliveries == 2 && "saved-action".equals(thread.delivered.getStringExtra("marker")),
                    "Restored action click did not dispatch its own persisted PendingIntent");
            System.out.println("NotificationSecurityTest: fresh controllers restored content/action clicks and creator identity");

            File altered = changedComment(new File(probePath), new File(data, "changed-click-owner"));
            method(pmType, "add", apk).invoke(reopenedPm, parse.invoke(null, altered.getPath()));
            require(load.invoke(null, pkg, key) == null, "Changed code loaded an old notification's capabilities");
            click.invoke(reopened, "n|"+key);
            require(thread.deliveries == 2, "Changed code received a stale notification click");
            Method send = method(store, "send", amType, restoredContent.getClass());
            require((Integer)send.invoke(null, reopenedAm, restoredContent) == -1 && thread.deliveries == 2,
                    "Changed creator code consumed a previously decoded stored capability");
            System.out.println("NotificationSecurityTest: changed code blocked before stored click/capability dispatch");
        } finally { last.set(null, prior); }
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        File data = new File(System.getenv("ANDROID_DATA"));
        data.mkdirs();
        File isolated = Files.createTempDirectory(data.toPath(), "notification-policy-").toFile();
        android.system.Os.setenv("ANDROID_DATA", isolated.getAbsolutePath(), true);
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        require(args.length == 2, "Pass microgprobe and devsvc fixture APK paths (no Google APK or network required)");
        Class<?> apk = Class.forName("com.gesher.server.ApkPackage");
        Method parse = apk.getMethod("parse", String.class);
        Object probe=parse.invoke(null,args[0]), dev=parse.invoke(null,args[1]);
        Class<?> pmType=Class.forName("com.gesher.server.PackageManagerService");
        Object pm=pmType.getConstructor(java.util.Collection.class).newInstance(List.of(probe,dev));
        Class<?> caller=Class.forName("com.gesher.server.CallerIdentity");
        Object identity=caller.getConstructor(pmType,Class.forName("com.gesher.server.ActivityTaskManagerService"))
                .newInstance(pm,null);
        Object policy=pmType.getMethod("permissions").invoke(pm);
        method(policy.getClass(),"attach",caller).invoke(policy,identity);
        Class<?> notifications=Class.forName("com.gesher.server.NotificationManagerService");
        AtomicInteger shown=new AtomicInteger(); Object service=service(notifications,pmType,pm,shown);
        String pkg="com.gesher.microgprobe", permission="android.permission.POST_NOTIFICATIONS";
        Method post=method(notifications,"post",String.class,String.class,int.class,Notification.class);
        Method active=notifications.getMethod("getAppActiveNotifications",String.class,int.class);
        Method enabled=notifications.getMethod("areNotificationsEnabled",String.class);
        Method grant=policy.getClass().getMethod("grantRuntimePermission",String.class,String.class,String.class,int.class);
        Method revoke=policy.getClass().getMethod("revokeRuntimePermission",String.class,String.class,String.class,int.class,String.class);
        Notification n=new Notification(); n.extras=new Bundle();
        n.extras.putString(Notification.EXTRA_TITLE,"non-personal notification fixture");
        revoke.invoke(policy,pkg,permission,"default:0",0,"test initial denial");
        require(!(boolean)enabled.invoke(service,pkg),"denied POST reported enabled");
        post.invoke(service,pkg,null,1,n);
        require(shown.get()==0 && ((ParceledListSlice<?>)active.invoke(service,pkg,0)).getList().isEmpty(),
                "denied notification leaked to toast/active queries");
        n.flags |= Notification.FLAG_FOREGROUND_SERVICE;
        post.invoke(service,pkg,null,2,n);
        require(shown.get()==0 && ((ParceledListSlice<?>)active.invoke(service,pkg,0)).getList().isEmpty(),
                "denied foreground notification content leaked");
        grant.invoke(policy,pkg,permission,"default:0",0);
        require((boolean)enabled.invoke(service,pkg),"explicit POST grant ignored");
        post.invoke(service,pkg,null,3,n);
        List<?> visible=((ParceledListSlice<?>)active.invoke(service,pkg,0)).getList();
        require(shown.get()==1 && visible.size()==1,"granted notification not posted");
        require(((StatusBarNotification)visible.get(0)).getUid()==(int)apk.getMethod("uid").invoke(probe),
                "notification published the server UID instead of its owner UID");
        revoke.invoke(policy,pkg,permission,"default:0",0,"test revoke");
        require(((ParceledListSlice<?>)active.invoke(service,pkg,0)).getList().isEmpty(),
                "POST revocation did not remove active content");
        post.invoke(service,pkg,null,4,n); require(shown.get()==1,"revoked POST still displayed");
        System.out.println("NotificationSecurityTest: POST denial, foreground-content suppression, explicit grant and live revoke passed");

        ComponentName component=new ComponentName("com.gesher.devsvc","com.gesher.devsvc.TestListener");
        Method allowed=notifications.getMethod("isNotificationListenerAccessGranted",ComponentName.class);
        Method setAccess=notifications.getMethod("setNotificationListenerAccessGranted",ComponentName.class,boolean.class,boolean.class);
        Method register=notifications.getMethod("registerListener",INotificationListener.class,ComponentName.class,int.class);
        Method read=notifications.getMethod("getActiveNotificationsFromListener",INotificationListener.class,String[].class,int.class);
        AtomicInteger callbacks=new AtomicInteger(); Binder binder=new Binder();
        INotificationListener listener=(INotificationListener)Proxy.newProxyInstance(
                INotificationListener.class.getClassLoader(),new Class<?>[]{INotificationListener.class},(proxy,m,values)-> {
                    if (m.getName().equals("asBinder")) return binder;
                    if (m.getName().equals("onNotificationPostedFull")) callbacks.incrementAndGet();
                    if (m.getName().equals("hashCode")) return System.identityHashCode(proxy);
                    if (m.getName().equals("equals")) return proxy==values[0];
                    return null;
                });
        require(!(boolean)allowed.invoke(service,component),"manifest listener auto-authorized");
        denied(()->register.invoke(service,listener,component,0));
        denied(()->read.invoke(service,null,null,0));
        denied(()->read.invoke(service,listener,null,0));
        setAccess.invoke(service,component,true,true);
        require((boolean)allowed.invoke(service,component),"explicit listener special access absent");
        register.invoke(service,listener,component,0);
        grant.invoke(policy,pkg,permission,"default:0",0); post.invoke(service,pkg,null,5,n);
        require(callbacks.get()==1,"approved listener did not receive notification");
        require(((ParceledListSlice<?>)read.invoke(service,listener,null,0)).getList().size()==1,
                "approved listener cannot query active notifications");
        setAccess.invoke(service,component,false,true);
        denied(()->read.invoke(service,listener,null,0));
        post.invoke(service,pkg,null,6,n);
        require(callbacks.get()==1,"revoked listener still received notification content");
        Object reopened=service(notifications,pmType,pm,shown);
        require(!(boolean)allowed.invoke(reopened,component),"listener revoke did not persist");
        setAccess.invoke(reopened,component,true,true);
        File changed=changedComment(new File(args[1]),new File(System.getenv("ANDROID_DATA"),"changed-listener"));
        Object replacement=parse.invoke(null,changed.getPath());
        method(pmType,"add",apk).invoke(pm,replacement);
        require(!(boolean)allowed.invoke(reopened,component),"unsigned replacement inherited listener access");
        denied(()->read.invoke(service,listener,null,0));
        System.out.println("NotificationSecurityTest: listener default-deny, explicit grant/revoke, token checks and replacement identity passed");

        Class<?> services=Class.forName("com.gesher.server.Services");
        java.lang.reflect.Field last=services.getDeclaredField("sLast"); last.setAccessible(true);
        Object prior=last.get(null);
        Class<?> created=Class.forName("com.gesher.server.Services$Created");
        Constructor<?> createdConstructor=created.getDeclaredConstructor(pmType,
                Class.forName("com.gesher.server.ActivityTaskManagerService"),
                Class.forName("com.gesher.server.ActivityManagerService"));
        createdConstructor.setAccessible(true);
        last.set(null,createdConstructor.newInstance(pm,null,null));
        try {
            Class<?> store=Class.forName("com.gesher.server.NotificationStore");
            Method save=method(store,"save",Class.forName("com.gesher.server.ActivityManagerService"),
                    String.class,String.class,String.class,int.class,Notification.class);
            Method load=method(store,"load",String.class,String.class);
            String key="0|"+pkg+"|75413|null|"+apk.getMethod("uid").invoke(probe);
            save.invoke(null,null,key,pkg,null,75413,n);
            require(load.invoke(null,pkg,key)!=null,"versioned notification record failed roundtrip");
            File alteredOwner=changedComment(new File(args[0]),new File(System.getenv("ANDROID_DATA"),"changed-owner"));
            Object changedOwner=parse.invoke(null,alteredOwner.getPath());
            method(pmType,"add",apk).invoke(pm,changedOwner);
            require(load.invoke(null,pkg,key)==null,"stored notification survived changed creator code");
            System.out.println("NotificationSecurityTest: stored notification owner identity checked before restoring capabilities");
        } finally { last.set(null,prior); }
        storedClickRestart(args[0], args[1], isolated, apk, parse, pmType, notifications, shown);
        System.out.println("NotificationSecurityTest all passed");
    }
}
