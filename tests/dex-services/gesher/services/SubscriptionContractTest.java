/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.telephony.SubscriptionManager;

import com.android.internal.telephony.ISub;

import java.lang.reflect.Constructor;

/** Checks the production absent-subscription service without requiring Android JNI. */
public final class SubscriptionContractTest {
    private static int checks;

    private static void expect(String name, int actual, int expected) {
        if (actual != expected) {
            throw new AssertionError(name + ": expected " + expected + ", got " + actual);
        }
        ++checks;
    }

    /** Also used through the service's real generated Binder by the manager fixture. */
    static int verifyService(ISub service) throws Exception {
        checks = 0;
        expect("default", service.getDefaultSubId(), SubscriptionManager.INVALID_SUBSCRIPTION_ID);
        expect("default data", service.getDefaultDataSubId(), SubscriptionManager.INVALID_SUBSCRIPTION_ID);
        expect("default voice", service.getDefaultVoiceSubId(), SubscriptionManager.INVALID_SUBSCRIPTION_ID);
        expect("default SMS", service.getDefaultSmsSubId(), SubscriptionManager.INVALID_SUBSCRIPTION_ID);
        expect("active data", service.getActiveDataSubscriptionId(), SubscriptionManager.INVALID_SUBSCRIPTION_ID);
        expect("preferred data without PhoneSwitcher", service.getPreferredDataSubscriptionId(),
                SubscriptionManager.INVALID_SUBSCRIPTION_ID);
        for (int user : new int[] {0, 10}) {
            expect("default as user " + user, service.getDefaultSubIdAsUser(user),
                    SubscriptionManager.INVALID_SUBSCRIPTION_ID);
            expect("voice as user " + user, service.getDefaultVoiceSubIdAsUser(user),
                    SubscriptionManager.INVALID_SUBSCRIPTION_ID);
            expect("SMS as user " + user, service.getDefaultSmsSubIdAsUser(user),
                    SubscriptionManager.INVALID_SUBSCRIPTION_ID);
        }
        for (int id : new int[] {-1, 0, 17, SubscriptionManager.DEFAULT_SUBSCRIPTION_ID}) {
            expect("subscription from slot " + id, service.getSubId(id),
                    SubscriptionManager.INVALID_SUBSCRIPTION_ID);
            expect("slot from subscription " + id, service.getSlotIndex(id),
                    SubscriptionManager.INVALID_SIM_SLOT_INDEX);
            expect("phone from subscription " + id, service.getPhoneId(id),
                    id < 0 || id == SubscriptionManager.DEFAULT_SUBSCRIPTION_ID
                            ? SubscriptionManager.INVALID_PHONE_INDEX : SubscriptionManager.DEFAULT_PHONE_INDEX);
        }
        for (boolean visible : new boolean[] {true, false}) {
            int[] ids = service.getActiveSubIdList(visible);
            expect("active IDs visible=" + visible, ids == null ? -1 : ids.length, 0);
        }
        return checks;
    }

    public static void main(String[] args) throws Exception {
        Class<?> type = Class.forName("com.gesher.server.DeviceServices$SubscriptionService");
        Constructor<?> constructor = type.getDeclaredConstructor();
        constructor.setAccessible(true);
        ISub service = (ISub) constructor.newInstance();
        System.out.println("SubscriptionContractTest all passed: " + verifyService(service));
    }
}
