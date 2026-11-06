/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.accounts.Account;
import android.os.Bundle;
import java.security.SecureRandom;
import java.util.Base64;
import java.util.HashMap;
import java.util.Map;
import java.util.function.LongSupplier;
import java.util.function.Predicate;

/** Server-private authenticator sessions. Clients receive no session/password/token data. */
final class AccountSessions {
    static final String HANDLE = "com.gesher.accounts.sessionHandle";
    private static final long LIFETIME_MS = 10 * 60 * 1000;
    private static final int MAX_PENDING = 1024;
    private final SecureRandom mRandom = new SecureRandom();
    private final LongSupplier mNow;
    private final Map<String, Pending> mPending = new HashMap<>();

    static final class Pending {
        final String client, owner, ownerCode, type;
        final int uid;
        final Account account;
        final long expires;
        final long ownerEpoch;
        private final Bundle data;
        Pending(String client, int uid, String owner, String ownerCode, String type,
                Account account, Bundle data, long expires, long ownerEpoch) {
            this.client = client; this.uid = uid; this.owner = owner; this.ownerCode = ownerCode;
            this.type = type; this.account = account; this.data = new Bundle(data); this.expires = expires;
            this.ownerEpoch = ownerEpoch;
        }
        Bundle data() { return new Bundle(data); }
    }

    AccountSessions() { this(android.os.SystemClock::elapsedRealtime); }
    AccountSessions(LongSupplier now) { mNow = now; }

    synchronized Bundle issue(String client, int uid, String owner, String ownerCode, String type,
            Account account, Bundle data) {
        return issue(client, uid, owner, ownerCode, type, account, data, 0);
    }

    synchronized Bundle issue(String client, int uid, String owner, String ownerCode, String type,
            Account account, Bundle data, long ownerEpoch) {
        if (client == null || owner == null || ownerCode == null || type == null || data == null) {
            throw new IllegalArgumentException("Incomplete account session identity");
        }
        long now = mNow.getAsLong();
        mPending.values().removeIf(p -> p.expires <= now);
        if (mPending.size() >= MAX_PENDING) throw new IllegalStateException("Too many account sessions");
        byte[] entropy = new byte[32];
        String handle;
        do {
            mRandom.nextBytes(entropy);
            handle = Base64.getUrlEncoder().withoutPadding().encodeToString(entropy);
        } while (mPending.containsKey(handle));
        mPending.put(handle, new Pending(client, uid, owner, ownerCode, type, account, data,
                now + LIFETIME_MS, ownerEpoch));
        Bundle result = new Bundle(); result.putString(HANDLE, handle); return result;
    }

    void packageRemoved(String packageName) {
        String prefix = packageName + "\u0000";
        synchronized (this) {
            mPending.values().removeIf(p -> packageName.equals(p.owner) || p.client.startsWith(prefix));
        }
    }

    Pending consume(Bundle publicSession, String client, int uid,
            Predicate<Pending> currentOwner) {
        if (publicSession == null || publicSession.size() != 1 || !publicSession.containsKey(HANDLE)) {
            throw new SecurityException("Invalid account session envelope");
        }
        String handle = publicSession == null ? null : publicSession.getString(HANDLE);
        Pending pending;
        synchronized (this) {
            pending = handle == null ? null : mPending.get(handle);
            // A wrong caller cannot invalidate a valid caller's opaque session.
            if (pending == null || !pending.client.equals(client) || pending.uid != uid) {
                throw new SecurityException("Invalid account session");
            }
        }
        // The owner check may take the account-store lock. Never invoke it while
        // holding this lock: removal holds the store lock before retiring sessions.
        boolean current = currentOwner.test(pending);
        synchronized (this) {
            if (mPending.get(handle) != pending || pending.expires <= mNow.getAsLong() || !current) {
                if (mPending.get(handle) == pending) mPending.remove(handle);
                throw new SecurityException("Account session expired or its owner changed");
            }
            mPending.remove(handle); // Consume before dispatch, including authenticator failures.
            return pending;
        }
    }
}
