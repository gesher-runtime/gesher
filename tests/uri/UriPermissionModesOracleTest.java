/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import com.android.server.uri.AospUriPermissionOracle;
import java.util.Arrays;

/** Compare production transitions with the pinned, unmodified Android implementation. */
public final class UriPermissionModesOracleTest {
    private static final int READ = 1, WRITE = 2, PERSISTABLE = 64;
    private static int checks;

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }

    private static final class Pair {
        final UriPermissionModes<Object> actual = new UriPermissionModes<>();
        final AospUriPermissionOracle expected = new AospUriPermissionOracle();
        final Object[] owners = { new Object(), new Object() };

        void compare(String operation) {
            int[] actualModes = {actual.active(), actual.owned(), actual.global,
                    actual.persistable, actual.persisted};
            check(Arrays.equals(actualModes, expected.modes()), operation + ": modes differ: "
                    + Arrays.toString(actualModes) + " vs " + Arrays.toString(expected.modes()));
            for (int modes = 0; modes <= (READ | WRITE); modes++) {
                check(actual.strength(modes) == expected.strength(modes), operation + ": strength differs");
            }
            check((actual.persistedTime == Long.MIN_VALUE) == (expected.time() == Long.MIN_VALUE),
                    operation + ": invalid-time state differs");
        }
        void grant(int modes, int owner) {
            actual.grant(modes, owner < 0 ? null : owners[owner]); expected.grant(modes, owner);
            compare("grant");
        }
        void take(int modes) {
            long before = System.currentTimeMillis();
            boolean changed = expected.take(modes);
            long after = System.currentTimeMillis();
            check(actual.take(modes, after) == changed, "take: persistence change differs");
            if (actual.persisted != 0) {
                check(expected.time() >= before && expected.time() <= after,
                        "AOSP take refreshes the persisted timestamp");
                check(actual.persistedTime == after, "Production take refreshes the persisted timestamp");
            }
            compare("take");
        }
        void release(int modes) {
            check(actual.release(modes) == expected.release(modes), "release: persistence change differs");
            compare("release");
        }
        void revoke(int modes, boolean ownersToo) {
            check(actual.revoke(modes, ownersToo) == expected.revoke(modes, ownersToo),
                    "revoke: persistence change differs");
            compare("revoke");
        }
        void retire(int owner, int modes) {
            actual.removeOwner(owners[owner], modes); expected.retire(owner, modes);
            compare("owner retirement");
        }
    }

    public static void main(String[] args) {
        System.load(args[0]);
        int[] grants = {READ, WRITE, READ | WRITE, READ | PERSISTABLE,
                WRITE | PERSISTABLE, READ | WRITE | PERSISTABLE};
        for (int first : grants) for (int firstOwner = -1; firstOwner < 2; firstOwner++) {
            for (int second : grants) for (int secondOwner = -1; secondOwner < 2; secondOwner++) {
                Pair pair = new Pair();
                pair.grant(first, firstOwner); pair.grant(second, secondOwner);
                pair.take(pair.actual.persistable);
                pair.take(0);
                pair.grant(READ | PERSISTABLE, 0); pair.take(READ);
                pair.retire(0, READ); pair.release(READ);
                pair.retire(1, WRITE); pair.revoke(WRITE | PERSISTABLE, false);
                pair.revoke(READ | WRITE, true); pair.release(READ | WRITE);
            }
        }
        for (int modes = READ; modes <= (READ | WRITE); modes++) {
            Pair pair = new Pair();
            pair.actual.restore(modes, 12345); pair.expected.restore(modes, 12345);
            pair.compare("restored state");
            check(pair.actual.active() == modes && pair.actual.global == 0 && pair.actual.owned() == 0,
                    "Restoration creates only durable access");
            pair.take(modes); pair.release(modes);
        }
        Pair offers = new Pair();
        offers.grant(READ | PERSISTABLE, 0);
        offers.grant(WRITE | PERSISTABLE, 1);
        offers.take(WRITE); offers.retire(0, READ);
        check(offers.actual.active() == WRITE, "An offer is excluded from active mode flags");
        offers.retire(1, WRITE); offers.release(WRITE);
        System.out.println("UriPermissionModesOracleTest passed: " + checks
                + " assertions against pinned AOSP UriPermission/UriPermissionOwner");
    }
}
