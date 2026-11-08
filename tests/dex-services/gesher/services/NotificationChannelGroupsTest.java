/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.INotificationManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationChannelGroup;
import android.app.NotificationManager;
import android.app.PropertyInvalidatedCache;
import android.app.Service;
import android.app.job.IJobCallback;
import android.app.job.JobInfo;
import android.content.ComponentName;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.pm.ParceledListSlice;
import android.content.pm.ServiceInfo;
import android.content.res.AssetManager;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;
import android.service.notification.StatusBarNotification;
import android.service.notification.INotificationListener;
import android.service.notification.NotificationRankingUpdate;
import com.gesher.server.NotificationManagerService;
import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.time.InstantSource;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.ToIntBiFunction;

/** Real framework manager -> AIDL Parcel -> service, plus policy persistence and publication races. */
public final class NotificationChannelGroupsTest {
    private static int checks;
    private interface Work { void run() throws Exception; }
    private static void require(boolean yes, String why) { if (!yes) throw new AssertionError(why); checks++; }
    private static void rejects(Class<? extends Throwable> type, Work work, String why) throws Exception {
        try { work.run(); }
        catch (Throwable error) {
            if (error instanceof InvocationTargetException) error = error.getCause();
            if (type.isInstance(error)) { checks++; return; }
            throw new AssertionError(why + ": wrong failure", error);
        }
        throw new AssertionError(why);
    }
    private static Field field(Class<?> cls, String name) throws Exception {
        Field value = cls.getDeclaredField(name); value.setAccessible(true); return value;
    }
    private static Method method(Class<?> cls, String name, Class<?>... types) throws Exception {
        Method value = cls.getDeclaredMethod(name, types); value.setAccessible(true); return value;
    }
    private static Object make(Class<?> cls, Class<?>[] types, Object... args) throws Exception {
        Constructor<?> value = cls.getDeclaredConstructor(types); value.setAccessible(true); return value.newInstance(args);
    }
    private static NotificationChannel channel(String id, String group, int importance) {
        NotificationChannel value = new NotificationChannel(id, "Channel " + id, importance);
        value.setDescription("description " + id); value.setGroup(group); return value;
    }
    private static NotificationChannelGroup group(String id, String name) {
        NotificationChannelGroup value = new NotificationChannelGroup(id, name);
        value.setDescription("description " + id); return value;
    }
    public static NotificationManager manager(INotificationManager service, String pkg) {
        Context context = new ContextWrapper(null) {
            @Override public String getPackageName() { return pkg; }
            @Override public String getOpPackageName() { return pkg; }
            @Override public int getUserId() { return 0; }
        };
        NotificationManager manager = new NotificationManager(context, InstantSource.system()) {
            @Override protected INotificationManager service() { return service; }
        };
        manager.setChannelCachesToTestMode();
        return manager;
    }

    /** Also executed on a JDK with real framework classes and explicitly modeled native boundaries. */
    public static void contracts(NotificationManager nm, INotificationManager wire,
            NotificationManagerService direct, String pkg) throws Exception {
        require(nm.getNotificationChannelGroups().isEmpty(), "New package groups are not empty");
        nm.createNotificationChannelGroup(group("manager-probe", "Public manager round trip"));
        require(nm.getNotificationChannelGroup("manager-probe") != null,
                "Public NotificationManager lost a successfully created notification channel group");
        nm.deleteNotificationChannelGroup("manager-probe");
        require(nm.getNotificationChannelGroup("manager-probe") == null, "Public manager group deletion failed");
        require(wire.getNotificationChannelGroups(pkg).getList().isEmpty(), "Direct list returned null or nonempty");
        require(wire.getNotificationChannelGroupsWithoutChannels(pkg).getList().isEmpty(), "Header list returned null or nonempty");
        require(nm.getNotificationChannelGroup("absent") == null, "Unknown group returned a fabricated object");
        legacyImportance();
        NotificationChannelGroup supplied = group("accounts", "Accounts – original");
        supplied.setBlocked(true); supplied.lockFields(NotificationChannelGroup.USER_LOCKED_BLOCKED_STATE);
        supplied.addChannel(channel("injected", "accounts", 3));
        direct.createNotificationChannelGroups(pkg, new ParceledListSlice<>(List.of(supplied)));
        supplied.setDescription("mutated input"); supplied.setBlocked(true);
        nm.createNotificationChannelGroup(group("empty", "Empty group"));
        NotificationChannelGroup created = nm.getNotificationChannelGroup("accounts");
        require(created != null && "description accounts".equals(created.getDescription()), "Input aliased service group storage");
        require(!created.isBlocked() && created.getUserLockedFields() == 0, "App selected blocked or user-locked group policy");
        require(created.getChannels().isEmpty() && nm.getNotificationChannel("injected") == null, "Embedded group members created channels");
        require(nm.getNotificationChannelGroups().size() == 2, "Empty group disappeared from manager query");

        NotificationChannel first = channel("mail", "accounts", NotificationManager.IMPORTANCE_HIGH);
        first.lockFields(NotificationChannel.USER_LOCKED_IMPORTANCE);
        first.setVibrationPattern(new long[]{0, 40, 20, 40});
        first.enableLights(true); first.setLightColor(0xff123456);
        direct.createNotificationChannels(pkg, new ParceledListSlice<>(List.of(first)));
        first.setGroup("empty"); first.setName("mutated input");
        first.getVibrationPattern()[1] = 900;
        ((android.os.VibrationEffect.Composed)first.getVibrationEffect()).getSegments().clear();
        require(nm.getNotificationChannelGroup("accounts").getChannels().size() == 1, "Cached group ignored channel creation");
        require(nm.getNotificationChannel("mail").getUserLockedFields() == 0, "App forged channel user locks");
        require("accounts".equals(nm.getNotificationChannel("mail").getGroup()), "Channel input aliased service state");
        require(nm.getNotificationChannel("mail").getVibrationPattern()[1] == 40, "Vibration input array aliased service storage");
        require(!((android.os.VibrationEffect.Composed)nm.getNotificationChannel("mail").getVibrationEffect()).getSegments().isEmpty(),
                "Input vibration effect aliased service storage");
        NotificationChannel invalidVibration = channel("invalid-vibration", null, 3);
        long[] invalidPattern = {0, 0};
        invalidVibration.setVibrationPattern(invalidPattern);
        require(invalidVibration.getVibrationPattern() != null && invalidVibration.getVibrationEffect() == null,
                "Fixture did not create a preserved invalid pattern with no effect");
        direct.createNotificationChannels(pkg, new ParceledListSlice<>(List.of(invalidVibration)));
        invalidPattern[1] = 40;
        NotificationChannel invalidResult = direct.getNotificationChannel(pkg, 0, pkg, "invalid-vibration");
        require(java.util.Arrays.equals(invalidResult.getVibrationPattern(), new long[]{0, 0})
                && invalidResult.getVibrationEffect() == null,
                "Copy discarded an invalid non-null vibration pattern or aliased its input");
        invalidResult.getVibrationPattern()[1] = 80;
        require(java.util.Arrays.equals(direct.getNotificationChannel(pkg, 0, pkg, "invalid-vibration")
                .getVibrationPattern(), new long[]{0, 0}), "Invalid vibration output array aliased server state");
        nm.createNotificationChannels(List.of(channel("chat", "accounts", 3), channel("ungrouped", null, 3)));
        require(nm.getNotificationChannelGroup("accounts").getChannels().size() == 2, "Cached members did not refresh");
        require(nm.getNotificationChannelGroups().size() == 2, "Ungrouped channel created an extra public group");
        for (Object value : wire.getNotificationChannelGroupsWithoutChannels(pkg).getList()) {
            require(((NotificationChannelGroup)value).getChannels().isEmpty(), "Cache header leaked member payloads");
        }
        NotificationChannelGroup renamed = group("accounts", "Renamed accounts");
        renamed.setBlocked(true); renamed.lockFields(NotificationChannelGroup.USER_LOCKED_BLOCKED_STATE);
        nm.createNotificationChannelGroup(renamed);
        require("Renamed accounts".contentEquals(nm.getNotificationChannelGroup("accounts").getName()), "Group cache hid rename");
        require(nm.getNotificationChannelGroup("accounts").getChannels().size() == 2, "Rename discarded member channels");
        require(!nm.getNotificationChannelGroup("accounts").isBlocked(), "Rename changed blocked policy");

        NotificationChannelGroup snapshot = direct.getNotificationChannelGroup(pkg, "accounts");
        snapshot.setDescription("mutated output"); snapshot.getChannels().get(0).setName("mutated child");
        snapshot.getChannels().clear();
        NotificationChannel output = direct.getNotificationChannel(pkg, 0, pkg, "mail");
        output.setGroup("empty"); output.setImportance(0);
        output.getVibrationPattern()[1] = 700;
        ((android.os.VibrationEffect.Composed)output.getVibrationEffect()).getSegments().clear();
        require(direct.getNotificationChannelGroup(pkg, "accounts").getChannels().size() == 2, "Output group/list aliased service storage");
        require("description accounts".equals(direct.getNotificationChannelGroup(pkg, "accounts").getDescription()), "Output metadata aliased service storage");
        require("Channel mail".contentEquals(nm.getNotificationChannel("mail").getName()), "Output child aliased service storage");
        require(nm.getNotificationChannel("mail").getImportance() == 4, "Direct channel output changed policy");
        require(direct.getNotificationChannel(pkg, 0, pkg, "mail").getVibrationPattern()[1] == 40, "Vibration output array aliased storage");
        require(!((android.os.VibrationEffect.Composed)direct.getNotificationChannel(pkg, 0, pkg, "mail").getVibrationEffect()).getSegments().isEmpty(),
                "Output vibration effect aliased service storage");
        nm.createNotificationChannel(channel("mail", "empty", NotificationManager.IMPORTANCE_MAX));
        require("accounts".equals(nm.getNotificationChannel("mail").getGroup()), "Existing channel moved between groups");
        require(nm.getNotificationChannel("mail").getImportance() == 4, "App raised an existing channel's importance");
        nm.createNotificationChannel(channel("mail", "accounts", NotificationManager.IMPORTANCE_LOW));
        require(nm.getNotificationChannel("mail").getImportance() == 2, "Permitted importance downgrade was ignored");
        require(nm.getNotificationChannel("mail").shouldShowLights(), "Metadata update reset existing channel settings");

        rejects(IllegalArgumentException.class, () -> nm.createNotificationChannel(channel("bad", "missing", 3)), "Channel accepted a missing group");
        NotificationChannel orphan = channel("orphan", null, 3);
        orphan.setConversationId("missing-parent", "conversation");
        rejects(IllegalArgumentException.class, () -> nm.createNotificationChannel(orphan), "Conversation channel accepted a missing parent");
        rejects(IllegalArgumentException.class, () -> nm.createNotificationChannelGroup(group("bad", "")), "Empty group name accepted");
        rejects(NullPointerException.class, () -> nm.createNotificationChannelGroup(group(null, "Null id")), "Null group ID accepted");
        rejects(IllegalArgumentException.class, () -> nm.createNotificationChannel(channel(NotificationChannel.DEFAULT_CHANNEL_ID, null, 3)), "Reserved channel ID created");
        rejects(IllegalArgumentException.class, () -> nm.deleteNotificationChannel(NotificationChannel.DEFAULT_CHANNEL_ID), "Default channel deleted");
        rejects(SecurityException.class, () -> wire.getNotificationChannelGroups("unknown.notification.owner"), "Unknown owner queried");
        require(nm.getNotificationChannel("bad") == null && nm.getNotificationChannelGroup("bad") == null, "Rejected input changed state");
        nm.deleteNotificationChannelGroup("accounts");
        require(nm.getNotificationChannelGroup("accounts") == null, "Deleted group survived cached query");
        require(nm.getNotificationChannel("mail") == null && nm.getNotificationChannel("chat") == null, "Group deletion kept members");
        require(nm.getNotificationChannelGroup("empty") != null && nm.getNotificationChannel("ungrouped") != null, "Group deletion touched unrelated state");
        require(wire.getNotificationChannelGroup(pkg, "accounts") == null, "Noncached group query retained deletion");
        nm.deleteNotificationChannelGroup("accounts"); nm.deleteNotificationChannelGroup("unknown");
        require(nm.getNotificationChannelGroups().size() == 1, "Idempotent deletion changed unrelated groups");
        nm.createNotificationChannelGroup(group("accounts", "Restored accounts"));
        require(nm.getNotificationChannelGroup("accounts").getChannels().isEmpty(), "Recreating group resurrected deleted channels");
        nm.createNotificationChannel(channel("mail", "accounts", 5));
        require(nm.getNotificationChannel("mail").getImportance() == 2, "Channel recreation discarded retained settings");
        require(nm.getNotificationChannelGroup("accounts").getChannels().size() == 1, "Restored channel missing from group");
        nm.deleteNotificationChannel("mail");
        require(nm.getNotificationChannelGroup("accounts").getChannels().isEmpty(), "Channel deletion did not invalidate member cache");
        nm.createNotificationChannel(channel("mail", "accounts", 5));
        require(nm.getNotificationChannel("mail").getImportance() == 2, "Individual deletion lost policy tombstone");
    }

    private static void legacyImportance() throws Exception {
        Method calculate = method(NotificationManagerService.class, "notificationImportance",
                NotificationChannel.class, Notification.class, boolean.class);
        NotificationChannel legacy = channel(NotificationChannel.DEFAULT_CHANNEL_ID, null, NotificationManager.IMPORTANCE_UNSPECIFIED);
        Notification n = new Notification();
        require((Integer)calculate.invoke(null, legacy, n, true) == NotificationManager.IMPORTANCE_LOW, "Quiet legacy default should be low importance");
        n.priority = Notification.PRIORITY_MIN;
        require((Integer)calculate.invoke(null, legacy, n, true) == NotificationManager.IMPORTANCE_MIN, "Quiet legacy minimum priority lost");
        n.defaults = Notification.DEFAULT_SOUND;
        require((Integer)calculate.invoke(null, legacy, n, true) == NotificationManager.IMPORTANCE_DEFAULT, "Noisy legacy notification did not reach default importance");
        n.flags = Notification.FLAG_HIGH_PRIORITY;
        require((Integer)calculate.invoke(null, legacy, n, true) == NotificationManager.IMPORTANCE_HIGH, "Legacy high-priority flag ignored");
        n.flags = 0; n.defaults = 0; n.priority = Notification.PRIORITY_MAX; n.vibrate = new long[]{0, 0};
        require((Integer)calculate.invoke(null, legacy, n, true) == NotificationManager.IMPORTANCE_LOW, "Invalid vibration was counted as noisy");
        n.priority = Notification.PRIORITY_MIN; n.vibrate = new long[]{0, 40};
        require((Integer)calculate.invoke(null, legacy, n, true) == NotificationManager.IMPORTANCE_DEFAULT, "Valid legacy waveform was not noisy");
        legacy.setImportance(NotificationManager.IMPORTANCE_NONE); legacy.lockFields(NotificationChannel.USER_LOCKED_IMPORTANCE);
        require((Integer)calculate.invoke(null, legacy, n, true) == NotificationManager.IMPORTANCE_NONE, "Legacy user-blocked channel lost its policy");
        NotificationChannel named = channel("named", null, NotificationManager.IMPORTANCE_HIGH);
        n.defaults = 0; n.vibrate = null;
        require((Integer)calculate.invoke(null, named, n, true) == NotificationManager.IMPORTANCE_HIGH, "Named channel used legacy notification priority");
    }

    public static void persistence(NotificationManager nm, NotificationManagerService service, String pkg) throws Exception {
        require("Restored accounts".contentEquals(nm.getNotificationChannelGroup("accounts").getName()), "Restart lost group metadata");
        NotificationChannel channel = nm.getNotificationChannel("mail");
        require(channel != null && channel.getImportance() == 2 && channel.shouldShowLights()
                && channel.getLightColor() == 0xff123456, "Restart lost channel policy");
        require(nm.getNotificationChannel("chat") == null, "Restart resurrected a deleted channel");
        NotificationChannel invalidVibration = service.getNotificationChannel(pkg, 0, pkg, "invalid-vibration");
        require(invalidVibration != null && java.util.Arrays.equals(invalidVibration.getVibrationPattern(), new long[]{0, 0})
                && invalidVibration.getVibrationEffect() == null, "Restart lost invalid-pattern/null-effect state");
        require(nm.getNotificationChannelGroup("accounts").getChannels().size() == 1, "Restart lost group membership");
        File blocked = new File(System.getenv("ANDROID_DATA"), "system/notification-channels/" + pkg + ".xml.new");
        Files.createDirectory(blocked.toPath());
        try {
            rejects(IllegalStateException.class, () -> nm.createNotificationChannelGroup(group("unwritten", "Cannot commit")), "Unwritable policy file reported success");
            require(service.getNotificationChannelGroup(pkg, "unwritten") == null, "Failed persistence published uncommitted state");
        } finally { Files.delete(blocked.toPath()); }
    }

    private static INotificationManager wire(NotificationManagerService service) throws Exception {
        Class<?> standIn = Class.forName("com.gesher.server.StandIn");
        standIn.getMethod("setStrict", boolean.class).invoke(null, true);
        Object stub = standIn.getMethod("of", Class.class, IInterface.class).invoke(null, INotificationManager.class, service);
        IBinder target = (IBinder)standIn.getMethod("asBinder").invoke(stub);
        return INotificationManager.Stub.asInterface(new Binder() {
            @Override protected boolean onTransact(int code, Parcel request, Parcel reply, int flags) throws RemoteException {
                return target.transact(code, request, reply, flags);
            }
        });
    }

    private static final class Fixture {
        final Class<?> apkType = Class.forName("com.gesher.server.ApkPackage");
        final Class<?> pmType = Class.forName("com.gesher.server.PackageManagerService");
        final Class<?> atmType = Class.forName("com.gesher.server.ActivityTaskManagerService");
        final Class<?> amType = Class.forName("com.gesher.server.ActivityManagerService");
        final Object owner, pm, atm;
        final String pkg;
        final AtomicInteger shown = new AtomicInteger();
        Object am;
        NotificationManagerService service;
        INotificationManager wire;
        NotificationManager nm;
        Fixture(String apk) throws Exception {
            owner = apkType.getMethod("parse", String.class).invoke(null, apk);
            pkg = (String)apkType.getField("packageName").get(owner);
            pm = pmType.getConstructor(Collection.class).newInstance(List.of(owner));
            atm = make(atmType, new Class<?>[]{pmType}, pm);
            Object policy = pmType.getMethod("permissions").invoke(pm);
            policy.getClass().getMethod("grantRuntimePermission", String.class, String.class, String.class, int.class)
                    .invoke(policy, pkg, "android.permission.POST_NOTIFICATIONS", "default:0", 0);
            restart();
        }
        void restart() throws Exception {
            ToIntBiFunction<StatusBarNotification,Boolean> sink = (n, update) -> { shown.incrementAndGet(); return 0; };
            service = (NotificationManagerService)make(NotificationManagerService.class,
                    new Class<?>[]{pmType, amType, ToIntBiFunction.class}, pm, null, sink);
            am = make(amType, new Class<?>[]{atmType, pmType, NotificationManagerService.class}, atm, pm, service);
            Class<?> created = Class.forName("com.gesher.server.Services$Created");
            field(Class.forName("com.gesher.server.Services"), "sLast").set(null,
                    make(created, new Class<?>[]{pmType, atmType, amType}, pm, atm, am));
            wire = wire(service); nm = manager(wire, pkg);
        }
        Notification notification(String channel) throws Exception {
            Notification value = new Notification(); field(Notification.class, "mChannelId").set(value, channel);
            value.setSmallIcon(android.graphics.drawable.Icon.createWithResource(pkg, android.R.drawable.ic_dialog_info));
            value.extras = new android.os.Bundle(); value.extras.putString(Notification.EXTRA_TITLE, "Synthetic channel contract");
            return value;
        }
        void post(String channel, int id) throws Exception {
            method(NotificationManagerService.class, "post", String.class, String.class, int.class, Notification.class)
                    .invoke(service, pkg, null, id, notification(channel));
        }
        int posted() throws Exception { synchronized(service) { return ((Map<?,?>)field(NotificationManagerService.class,"mPosted").get(service)).size(); } }
        int stored() {
            File[] values = new File(System.getenv("ANDROID_DATA"), "system/notifications/" + pkg).listFiles((dir,name)->name.endsWith(".n"));
            return values == null ? 0 : values.length;
        }
    }

    private static void await(CountDownLatch latch, String why) {
        try { require(latch.await(5, TimeUnit.SECONDS), why); }
        catch (InterruptedException error) { throw new AssertionError(error); }
    }
    private static void legacyRouting(Fixture f) throws Exception {
        int shown = f.shown.get();
        f.post("missing-modern", 91); f.post(null, 92);
        require(f.shown.get() == shown && f.posted() == 0 && f.stored() == 0,
                "Modern missing channel fabricated a default and published content");
        Object parsed = f.apkType.getField("parsed").get(f.owner);
        Method target = parsed.getClass().getMethod("setTargetSdkVersion", int.class);
        int originalTarget = (Integer)parsed.getClass().getMethod("getTargetSdkVersion").invoke(parsed);
        try {
            target.invoke(parsed, 25);
            f.restart();
            NotificationChannel legacy = f.nm.getNotificationChannel(NotificationChannel.DEFAULT_CHANNEL_ID);
            require(legacy != null && legacy.getImportance() == NotificationManager.IMPORTANCE_UNSPECIFIED,
                    "Legacy app did not receive a real unspecified-importance default channel");
            f.post(null, 90);
            require(f.shown.get() == shown + 1 && f.posted() == 1 && f.stored() == 1,
                    "Legacy null-channel notification did not use its real default channel");
            require(f.nm.getNotificationChannel(NotificationChannel.DEFAULT_CHANNEL_ID).getImportance()
                    == NotificationManager.IMPORTANCE_UNSPECIFIED, "Per-notification importance overwrote stored legacy policy");
            f.post("missing-legacy", 93);
            require(f.shown.get() == shown + 1 && f.posted() == 1, "Unknown named legacy channel silently used default");
        } finally { target.invoke(parsed, originalTarget); f.restart(); }
        require(f.nm.getNotificationChannel(NotificationChannel.DEFAULT_CHANNEL_ID) == null,
                "Upgrade to modern target retained legacy default channel");
        f.wire.cancelNotificationWithTag(f.pkg, f.pkg, null, 90, 0);
        require(f.stored() == 0, "Legacy routing fixture left retained notification state");
    }

    private static void retainedV2(Fixture f) throws Exception {
        Notification notification = f.notification("ungrouped");
        int uid = (Integer)f.apkType.getMethod("uid").invoke(f.owner);
        String key = new StatusBarNotification(f.pkg, f.pkg, 61, null, uid, 0, notification,
                android.os.UserHandle.SYSTEM, null, 0).getKey();
        String identity = (String)method(NotificationManagerService.class, "codeIdentity", f.apkType).invoke(null, f.owner);
        Parcel parcel = Parcel.obtain();
        try {
            parcel.writeInt(2); parcel.writeString(key); parcel.writeString(f.pkg); parcel.writeString(identity);
            parcel.writeString(null); parcel.writeInt(61); parcel.writeInt(Notification.FLAG_ONLY_ALERT_ONCE);
            parcel.writeInt(0); parcel.writeInt(0); // No content/action intents.
            File directory = new File(System.getenv("ANDROID_DATA"), "system/notifications/" + f.pkg);
            Files.createDirectories(directory.toPath());
            Files.write(new File(directory, Integer.toHexString(key.hashCode()) + ".n").toPath(), parcel.marshall());
        } finally { parcel.recycle(); }
        Class<?> store = Class.forName("com.gesher.server.NotificationStore");
        Object restored = method(store, "load", String.class, String.class).invoke(null, f.pkg, key);
        require(restored != null && field(restored.getClass(), "id").getInt(restored) == 61,
                "Store v3 reader rejected valid version-2 retained records");
        require(field(restored.getClass(), "channelId").get(restored) == null,
                "Version-2 migration fabricated unknown channel identity");
        f.wire.cancelNotificationWithTag(f.pkg, f.pkg, null, 61, 0);
    }

    private static void blockedChannels(Fixture f) throws Exception {
        int before = f.shown.get();
        f.nm.createNotificationChannel(channel("blocked", null, NotificationManager.IMPORTANCE_NONE));
        f.post("blocked", 94);
        require(f.shown.get() == before && f.posted() == 0 && f.stored() == 0,
                "Blocked channel leaked ordinary notification into active/store/toast state");
        f.nm.createNotificationChannel(channel("block-visible", null, NotificationManager.IMPORTANCE_DEFAULT));
        f.post("block-visible", 95);
        require(f.posted() == 1 && f.stored() == 1, "User-block fixture did not begin with visible content");
        NotificationChannel policy = f.nm.getNotificationChannel("block-visible");
        policy.setImportance(NotificationManager.IMPORTANCE_NONE);
        int uid = (Integer)f.apkType.getMethod("uid").invoke(f.owner);
        rejects(SecurityException.class, () -> f.wire.updateNotificationChannelForPackage(f.pkg, uid + 1, policy),
                "User policy update accepted wrong UID");
        f.wire.updateNotificationChannelForPackage(f.pkg, uid, policy);
        require(f.posted() == 0 && f.stored() == 0, "User channel block retained active notification or stored toast");
        NotificationChannel blocked = f.nm.getNotificationChannel("block-visible");
        require(blocked.getImportance() == 0 && blocked.hasUserSetImportance(), "User policy block/cache/importance lock was not stored");
        f.nm.createNotificationChannel(channel("block-visible", null, NotificationManager.IMPORTANCE_HIGH));
        f.post("block-visible", 96);
        require(f.posted() == 0 && f.shown.get() == before + 1, "App reset user policy or republished blocked content");
        blocked.setImportance(NotificationManager.IMPORTANCE_DEFAULT);
        f.wire.updateNotificationChannelForPackage(f.pkg, uid, blocked);
        f.post("block-visible", 97);
        require(f.posted() == 1 && f.shown.get() == before + 2, "User unblocking could not restore real publication");
        f.wire.cancelNotificationWithTag(f.pkg, f.pkg, null, 97, 0);
        f.nm.deleteNotificationChannel("blocked"); f.nm.deleteNotificationChannel("block-visible");
    }
    private static void publication(Fixture f) throws Exception {
        f.post("mail", 21); f.post("ungrouped", 22);
        require(f.posted() == 2 && f.stored() == 2, "Fixture did not publish real notification/store records");
        CountDownLatch unchangedCaptured = new CountDownLatch(1), unchangedResume = new CountDownLatch(1);
        AtomicReference<Throwable> unchangedFailure = new AtomicReference<>();
        field(NotificationManagerService.class, "mBeforePostPublication").set(f.service, (Runnable)() -> {
            unchangedCaptured.countDown(); await(unchangedResume, "Idempotent-create post not resumed");
        });
        int unchangedBefore = f.shown.get();
        Thread unchangedPost = new Thread(() -> {
            try { f.post("mail", 20); } catch (Throwable error) { unchangedFailure.set(error); }
        }, "idempotent-channel-post");
        unchangedPost.setDaemon(true); unchangedPost.start();
        try {
            await(unchangedCaptured, "Idempotent-create post did not pause");
            f.nm.createNotificationChannel(f.nm.getNotificationChannel("mail"));
        } finally { unchangedResume.countDown(); }
        unchangedPost.join(5000);
        require(!unchangedPost.isAlive() && unchangedFailure.get() == null, "Idempotent-create worker failed");
        require(f.shown.get() == unchangedBefore + 1 && f.posted() == 3,
                "Idempotent channel creation dropped a valid in-flight notification");
        CountDownLatch captured = new CountDownLatch(1), resume = new CountDownLatch(1);
        field(NotificationManagerService.class, "mBeforePostPublication").set(f.service, (Runnable)() -> {
            captured.countDown(); await(resume, "Paused post not resumed");
        });
        AtomicReference<Throwable> failure = new AtomicReference<>();
        int before = f.shown.get();
        Thread poster = new Thread(() -> { try { f.post("mail", 23); } catch (Throwable error) { failure.set(error); } }, "channel-deletion-post");
        poster.setDaemon(true); poster.start();
        try {
            await(captured, "Post did not reach its publication fence");
            f.nm.deleteNotificationChannelGroup("accounts");
            require(f.posted() == 1 && f.stored() == 1, "Group deletion retained member records or removed unrelated notifications");
            f.nm.createNotificationChannelGroup(group("accounts", "Recreated during post"));
            f.nm.createNotificationChannel(channel("mail", "accounts", 3));
        } finally { resume.countDown(); }
        poster.join(5000);
        require(!poster.isAlive() && failure.get() == null, "Publication race worker failed: " + failure.get());
        require(f.shown.get() == before && f.posted() == 1 && f.stored() == 1, "Old in-flight post resurrected after group/channel recreation");
        field(NotificationManagerService.class, "mBeforePostPublication").set(f.service, null);
        f.post("mail", 24);
        require(f.posted() == 2 && f.shown.get() == before + 1, "New channel generation could not post");
        f.restart();
        require(f.posted() == 0 && f.stored() == 2, "Restart fixture did not retain old host-toast records");
        f.nm.deleteNotificationChannelGroup("accounts");
        require(f.stored() == 1, "Group deletion after restart did not selectively remove retained toast state");
    }

    @SuppressWarnings("unchecked")
    private static void activeUses(Fixture f) throws Exception {
        f.nm.createNotificationChannelGroup(group("active", "Active work"));
        f.nm.createNotificationChannels(List.of(channel("fgs", "active", 3), channel("uij", "active", 3)));
        Object services = field(f.amType, "mServices").get(f.am);
        Class<?> servicesType = services.getClass(), recordType = Class.forName(servicesType.getName() + "$ServiceRecord");
        ServiceInfo info = new ServiceInfo(); info.packageName = f.pkg; info.name = f.pkg + ".SyntheticForegroundService";
        Object record = make(recordType, new Class<?>[]{ServiceInfo.class, f.apkType}, info, f.owner);
        ComponentName component = new ComponentName(info.packageName, info.name);
        ((Map<ComponentName,Object>)field(servicesType,"mServices").get(services)).put(component, record);
        Method foreground = method(servicesType, "setServiceForeground", ComponentName.class, IBinder.class,
                int.class, Notification.class, int.class, int.class);
        Object publicationLock = field(NotificationManagerService.class, "mPublicationLock").get(f.service);
        Class<?> listenerType = Class.forName(NotificationManagerService.class.getName() + "$Listener");
        Object listener = make(listenerType, new Class<?>[]{NotificationManagerService.class, ComponentName.class}, f.service, null);
        AtomicInteger callbacks = new AtomicInteger();
        field(listenerType, "listener").set(listener, new INotificationListener.Default() {
            @Override public void onNotificationPostedFull(StatusBarNotification sbn, NotificationRankingUpdate ranking, long flags) {
                require(!Thread.holdsLock(publicationLock), "Foreground/job listener callback ran under publication lock");
                AtomicReference<Throwable> error = new AtomicReference<>();
                Thread followup = new Thread(() -> {
                    try { f.nm.createNotificationChannelGroup(group("listener", "Listener callback")); }
                    catch (Throwable failure) { error.set(failure); }
                }, "listener-channel-operation");
                followup.setDaemon(true); followup.start();
                try { followup.join(5000); } catch (InterruptedException failure) { throw new AssertionError(failure); }
                require(!followup.isAlive() && error.get() == null, "Listener callback blocked cross-thread channel CRUD");
                callbacks.incrementAndGet();
            }
        });
        ((List<Object>)field(NotificationManagerService.class, "mListeners").get(f.service)).add(listener);
        field(NotificationManagerService.class, "mListenersBound").setBoolean(f.service, true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "0", true);
        try { foreground.invoke(services, component, record, 31, f.notification("fgs"), 0, 0); }
        finally { android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true); }
        require(callbacks.get() == 1, "Foreground post did not exercise actual listener dispatch");
        rejects(SecurityException.class, () -> f.nm.deleteNotificationChannelGroup("active"), "Deleted a foreground-service group");
        rejects(SecurityException.class, () -> f.nm.deleteNotificationChannel("fgs"), "Deleted an active foreground-service channel");
        require(f.nm.getNotificationChannelGroup("active").getChannels().size() == 2, "Failed preflight partially deleted a group");
        foreground.invoke(services, component, record, 32, f.notification("ungrouped"), 0, 0);
        f.nm.deleteNotificationChannel("fgs");
        require(f.nm.getNotificationChannel("fgs") == null, "Replacing foreground notification retained its old association");
        foreground.invoke(services, component, record, 0, null, Service.STOP_FOREGROUND_DETACH, 0);
        require(!(Boolean)method(servicesType, "hasForegroundServiceNotification", String.class, String.class)
                .invoke(services, f.pkg, "ungrouped"), "stopForeground retained active-channel ownership");

        Class<?> schedulerType = Class.forName("com.gesher.server.JobSchedulerService");
        Object scheduler = make(schedulerType, new Class<?>[]{f.amType, f.atmType, f.pmType}, f.am, f.atm, f.pm);
        Thread background = (Thread)field(schedulerType, "mThread").get(scheduler);
        if (background != null) { background.interrupt(); background.join(5000); require(!background.isAlive(), "Scheduler fixture did not stop its polling thread"); }
        Class<?> jobType = Class.forName(schedulerType.getName() + "$Job"), runType = Class.forName(schedulerType.getName() + "$Run");
        JobInfo jobInfo = new JobInfo.Builder(8387, new ComponentName(f.pkg, f.pkg + ".TestJobService"))
                .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY).setUserInitiated(true).build();
        Object job = make(jobType, new Class<?>[]{schedulerType, String.class, String.class, JobInfo.class}, scheduler, f.pkg, null, jobInfo);
        Object run = make(runType, new Class<?>[]{schedulerType, jobType}, scheduler, job);
        field(jobType, "run").set(job, run);
        ((List<Object>)field(schedulerType,"mJobs").get(scheduler)).add(job);
        field(schedulerType, "mRunning").setInt(scheduler, 1);
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "0", true);
        try { ((IJobCallback)run).setNotification(8387, 41, f.notification("uij"),
                android.app.job.JobService.JOB_END_NOTIFICATION_POLICY_DETACH); }
        finally { android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true); }
        require(callbacks.get() == 2, "User-initiated job did not exercise actual listener dispatch");
        rejects(SecurityException.class, () -> f.nm.deleteNotificationChannelGroup("active"), "Deleted an active user-initiated-job group");
        rejects(SecurityException.class, () -> f.nm.deleteNotificationChannel("uij"), "Deleted an active user-initiated-job channel");
        ((IJobCallback)run).jobFinished(8387, false);
        f.nm.deleteNotificationChannelGroup("active");
        require(f.nm.getNotificationChannelGroup("active") == null, "Completed job kept its group undeletable");
        require(!(Boolean)method(schedulerType,"hasUserInitiatedJobNotification",String.class,String.class)
                .invoke(scheduler,f.pkg,"uij"), "Completed job retained its channel association");
    }

    public static void limits(NotificationManagerService service, String pkg) throws Exception {
        int existing = service.getNotificationChannelGroupsWithoutChannels(pkg).getList().size();
        List<NotificationChannelGroup> bulk = new ArrayList<>();
        for (int i = existing; i < 6000; i++) bulk.add(group("limit-" + i, "Group " + i));
        service.createNotificationChannelGroups(pkg, new ParceledListSlice<>(bulk));
        require(service.getNotificationChannelGroupsWithoutChannels(pkg).getList().size() == 6000, "Group bound rejected a valid batch");
        rejects(IllegalStateException.class, () -> service.createNotificationChannelGroups(pkg,
                new ParceledListSlice<>(List.of(group("overflow", "Beyond limit")))), "Group bound accepted overflow");
        service.createNotificationChannelGroups(pkg, new ParceledListSlice<>(List.of(group("accounts", "Rename at capacity"))));
        require(service.getNotificationChannelGroup(pkg, "accounts") != null, "Renaming existing group consumed a new slot");
    }

    public static int checks() { return checks; }
    public static void main(String[] args) throws Exception {
        require(args.length == 1, "Pass the devsvc APK requesting notification permission");
        require("1000".equals(System.getenv("GESHER_BINDER_UID")), "System-UID fixture needed for real retained notification state");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false, System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        File parent = new File(System.getenv("ANDROID_DATA")); Files.createDirectories(parent.toPath());
        File data = Files.createTempDirectory(parent.toPath(), "notification-groups-").toFile();
        android.system.Os.setenv("ANDROID_DATA", data.getPath(), true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true);
        PropertyInvalidatedCache.setTestMode(true);
        Field last = field(Class.forName("com.gesher.server.Services"), "sLast"); Object previous = last.get(null);
        try {
            Fixture f = new Fixture(args[0]);
            contracts(f.nm, f.wire, f.service, f.pkg);
            legacyRouting(f);
            blockedChannels(f);
            f.restart(); persistence(f.nm, f.service, f.pkg);
            publication(f); retainedV2(f); activeUses(f);
            f.nm.createNotificationChannelGroup(group("accounts", "For capacity check"));
            limits(f.service, f.pkg);
            method(NotificationManagerService.class, "packageRemoved", String.class).invoke(f.service, f.pkg);
            require(f.nm.getNotificationChannelGroups().isEmpty() && f.nm.getNotificationChannels().isEmpty(), "Package removal left cached channel state");
            f.restart();
            require(f.nm.getNotificationChannelGroups().isEmpty() && f.nm.getNotificationChannels().isEmpty(), "Uninstalled channel policy returned after restart");
        } finally { last.set(null, previous); PropertyInvalidatedCache.setTestMode(false); }
        System.out.println("NotificationChannelGroupsTest all passed: " + checks + " checks");
    }
}
