/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.app.NotificationManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationChannelGroup;
import android.content.pm.ParceledListSlice;
import android.app.PropertyInvalidatedCache;
import android.service.notification.StatusBarNotification;
import com.gesher.server.NotificationManagerService;
import gesher.services.NotificationChannelGroupsTest;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.lang.reflect.Field;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.ToIntBiFunction;

/** Real production service/framework Java; host-specific native boundaries are explicitly separate. */
public final class NotificationGroupsHost {
    private static final AtomicInteger shown = new AtomicInteger();
    private static int checks;
    private interface Work { void run() throws Exception; }
    private static void check(boolean yes, String why) { if (!yes) throw new AssertionError(why); checks++; }
    private static NotificationManagerService service() throws Exception {
        Constructor<NotificationManagerService> constructor = NotificationManagerService.class.getDeclaredConstructor(
                Class.forName("com.gesher.server.PackageManagerService"),
                Class.forName("com.gesher.server.ActivityManagerService"), ToIntBiFunction.class);
        constructor.setAccessible(true);
        ToIntBiFunction<StatusBarNotification, Boolean> sink = (notification, update) -> { shown.incrementAndGet(); return 0; };
        return constructor.newInstance(null, null, sink);
    }
    private static void post(NotificationManagerService service, String channel, int id) throws Exception {
        Notification n = new Notification();
        Field field = Notification.class.getDeclaredField("mChannelId"); field.setAccessible(true); field.set(n, channel);
        Method post = NotificationManagerService.class.getDeclaredMethod("post", String.class, String.class, int.class, Notification.class);
        post.setAccessible(true); post.invoke(service, "android", null, id, n);
    }
    private static int active(NotificationManagerService service) {
        return service.getAppActiveNotifications("android", 0).getList().size();
    }
    private static void pausedPost(NotificationManagerService service, int id, Work mutation, boolean accepted) throws Exception {
        CountDownLatch captured = new CountDownLatch(1), resume = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Field hook = NotificationManagerService.class.getDeclaredField("mBeforePostPublication"); hook.setAccessible(true);
        hook.set(service, (Runnable)() -> {
            captured.countDown();
            try { if (!resume.await(5, TimeUnit.SECONDS)) throw new AssertionError("Post was not resumed"); }
            catch (InterruptedException error) { throw new AssertionError(error); }
        });
        int before = shown.get();
        Thread thread = new Thread(() -> { try { post(service, "mail", id); } catch (Throwable error) { failure.set(error); } });
        thread.setDaemon(true); thread.start();
        try { check(captured.await(5, TimeUnit.SECONDS), "Post did not pause"); mutation.run(); }
        finally { resume.countDown(); }
        thread.join(5000); hook.set(service, null);
        check(!thread.isAlive() && failure.get() == null, "Post race failed: " + failure.get());
        check(shown.get() == before + (accepted ? 1 : 0), "Channel generation fence accepted/dropped the wrong post");
        check(active(service) == (accepted ? 1 : 0), "Channel generation fence left incorrect active state");
        if (accepted) service.cancelNotificationWithTag("android", "android", null, id, 0);
    }
    private static void publication(NotificationManagerService service) throws Exception {
        int before = shown.get();
        post(service, null, 90); post(service, "absent", 91);
        check(active(service) == 0 && shown.get() == before, "Missing channel fabricated visible state");
        service.createNotificationChannels("android", new ParceledListSlice<>(List.of(new NotificationChannel("host-visible", "Visible", 0))));
        post(service, "host-visible", 92);
        check(active(service) == 0 && shown.get() == before, "Blocked channel published active content");
        NotificationChannel policy = service.getNotificationChannel("android", 0, "android", "host-visible");
        policy.setImportance(3); service.updateNotificationChannelForPackage("android", 1000, policy);
        post(service, "host-visible", 93);
        check(active(service) == 1 && shown.get() == before + 1, "User-unblocked channel did not publish");
        policy.setImportance(0); service.updateNotificationChannelForPackage("android", 1000, policy);
        check(active(service) == 0, "User channel block left an existing active record");
        check(service.getNotificationChannel("android", 0, "android", "host-visible").hasUserSetImportance(), "User block did not lock importance");
        service.createNotificationChannels("android", new ParceledListSlice<>(List.of(new NotificationChannel("host-visible", "Visible", 5))));
        post(service, "host-visible", 94);
        check(active(service) == 0 && shown.get() == before + 1, "App reset blocked user policy");
        pausedPost(service, 95, () -> service.createNotificationChannels("android", new ParceledListSlice<>(List.of(
                service.getNotificationChannel("android", 0, "android", "mail")))), true);
        pausedPost(service, 96, () -> {
            service.deleteNotificationChannelGroup("android", "accounts");
            service.createNotificationChannelGroups("android", new ParceledListSlice<>(List.of(new NotificationChannelGroup("accounts", "Restored accounts"))));
            NotificationChannel channel = new NotificationChannel("mail", "Channel mail", 5); channel.setGroup("accounts");
            service.createNotificationChannels("android", new ParceledListSlice<>(List.of(channel)));
        }, false);
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        PropertyInvalidatedCache.setTestMode(true);
        NotificationManagerService service = service();
        NotificationChannelGroupsTest.contracts(NotificationChannelGroupsTest.manager(service, "android"),
                service, service, "android");
        service = service();
        NotificationChannelGroupsTest.persistence(NotificationChannelGroupsTest.manager(service, "android"),
                service, "android");
        publication(service);
        NotificationChannelGroupsTest.limits(service, "android");
        NotificationManager cached = NotificationChannelGroupsTest.manager(service, "android");
        check(cached.getNotificationChannelGroups().size() == 6000, "Cache setup missing");
        Method removed = NotificationManagerService.class.getDeclaredMethod("packageRemoved", String.class);
        removed.setAccessible(true); removed.invoke(service, "android");
        check(cached.getNotificationChannelGroups().isEmpty() && cached.getNotificationChannels().isEmpty(),
                "Package retirement left cached state");
        service = service();
        check(service.getNotificationChannelGroups("android").getList().isEmpty()
                && service.getNotificationChannels("android", "android", 0).getList().isEmpty(),
                "Package retirement did not delete durable policy");
        System.out.println("Notification group host checks: " + (NotificationChannelGroupsTest.checks() + checks));
        System.out.println("Real production Java/framework; no ART, Binder Parcel, Windows toast or service/job dispatch claim.");
    }
}
