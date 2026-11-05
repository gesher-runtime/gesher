/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.os.Binder;
import android.os.IBinder;
import android.os.IServiceCallback;
import android.os.IServiceManager;
import android.os.RemoteException;

import java.lang.reflect.Constructor;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Existing/future service registration, cancellation, reentrancy, and dead observers. */
public final class ServiceRegistrationCallbackTest {
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static final class Token extends Binder {
        final List<IBinder.DeathRecipient> recipients = new ArrayList<>();
        @Override public void linkToDeath(IBinder.DeathRecipient recipient, int flags) {
            recipients.add(recipient);
        }
        @Override public boolean unlinkToDeath(IBinder.DeathRecipient recipient, int flags) {
            return recipients.remove(recipient);
        }
        void die() {
            for (IBinder.DeathRecipient recipient : new ArrayList<>(recipients)) recipient.binderDied();
        }
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        Class<?> type = Class.forName("com.gesher.server.Services$ServiceManagerStandIn");
        Constructor<?> ctor = type.getDeclaredConstructor(Map.class);
        ctor.setAccessible(true);
        Binder first = new Binder();
        Map<String, IBinder> services = new LinkedHashMap<>();
        services.put("present", first);
        IServiceManager manager = (IServiceManager) ctor.newInstance(services);
        List<IBinder> observed = new ArrayList<>();
        Token token = new Token();
        IServiceCallback callback = new IServiceCallback.Stub() {
            @Override public IBinder asBinder() { return token; }
            @Override public void onRegistration(String name, IBinder binder) {
                require("present".equals(name) || "future".equals(name), "wrong registration name");
                observed.add(binder);
            }
        };
        manager.registerForNotifications("present", callback);
        require(observed.size() == 1 && observed.get(0) == first, "existing service not delivered");
        manager.registerForNotifications("present", callback);
        require(observed.size() == 1 && token.recipients.size() == 1, "duplicate observer retained");
        manager.registerForNotifications("future", callback);
        require(observed.size() == 1, "missing service fabricated a callback");
        Binder second = new Binder();
        manager.addService("future", second, false, 0);
        require(observed.size() == 2 && observed.get(1) == second, "future service not delivered");
        manager.unregisterForNotifications("future", callback);
        manager.addService("future", new Binder(), false, 0);
        require(observed.size() == 2 && token.recipients.size() == 1,
                "unregister retained a subscription or removed a different name");
        token.die();
        manager.addService("present", new Binder(), false, 0);
        require(observed.size() == 2 && token.recipients.isEmpty(), "dead callback retained");

        Token broken = new Token();
        manager.registerForNotifications("present", new IServiceCallback.Stub() {
            @Override public IBinder asBinder() { return broken; }
            @Override public void onRegistration(String name, IBinder binder) throws RemoteException {
                throw new RemoteException("test transport failure");
            }
        });
        require(broken.recipients.isEmpty(), "failed callback retained its death recipient");
        final int[] reentrant = {0};
        manager.registerForNotifications("present", new IServiceCallback.Stub() {
            @Override public void onRegistration(String name, IBinder binder) throws RemoteException {
                manager.unregisterForNotifications(name, this);
                reentrant[0]++;
            }
        });
        manager.addService("present", new Binder(), false, 0);
        require(reentrant[0] == 1, "callback could not unregister itself");
        require(manager.getDeclaredInstances("missing").length == 0
                && manager.getUpdatableNames("missing").length == 0, "null service metadata arrays");
        System.out.println("ServiceRegistrationCallbackTest all passed");
    }
}
