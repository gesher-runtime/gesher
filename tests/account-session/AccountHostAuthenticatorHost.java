/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.accounts.Account;
import android.accounts.AccountManager;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;

/** Actual HostRemoval and Answer callbacks/validation/commit, with constructed
 * authenticator metadata. No Java production method is replaced. Binding to an
 * Android service and successfully opening its Activity require Windows/ART. */
public final class AccountHostAuthenticatorHost {
    static final Account A = AccountRetirementHost.REMOVED;
    static Class<?> type(String name) throws Exception { return AccountRetirementHost.t("AccountManagerService$" + name); }
    static Object call(Object o, String name, Class<?>[] types, Object... args) throws Exception {
        return AccountRetirementHost.call(o, name, types, args);
    }
    static void check(boolean ok, String why) { AccountRetirementHost.check(ok, why); }
    static void scenario(String outcome) throws Exception {
        try (var h = new AccountRetirementHost.H()) {
            Class<?> service = h.accounts.getClass(), auth = type("Authenticator"), answerType = type("Answer");
            Object owner = AccountRetirementHost.make(auth,
                    new Class<?>[]{String.class, ComponentName.class, int.class, int.class, int.class, int.class, boolean.class},
                    A.type, new ComponentName(AccountRetirementHost.OWNER, AccountRetirementHost.OWNER + ".Authenticator"), 0, 0, 0, 0, false);
            Object entry = h.entries.get(A);
            Object session = AccountRetirementHost.make(type("HostRemoval"),
                    new Class<?>[]{service, Account.class, entry.getClass(), auth}, h.accounts, A, entry, owner);
            Object answer = AccountRetirementHost.make(answerType,
                    new Class<?>[]{service, auth}, h.accounts, owner);
            if (outcome.equals("error")) {
                call(answer, "onError", new Class<?>[]{int.class, String.class}, AccountManager.ERROR_CODE_CANCELED, "fixture canceled");
            } else {
                Bundle result = new Bundle();
                if (outcome.equals("intent") || outcome.equals("invalidIntent")) {
                    String target = outcome.equals("intent") ? AccountRetirementHost.OWNER : AccountRetirementHost.OTHER;
                    result.putParcelable(AccountManager.KEY_INTENT, new Intent().setComponent(new ComponentName(target, target + ".Confirm")));
                    // Boolean success must not override a required Activity continuation.
                    result.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, true);
                } else if (!outcome.equals("malformed")) result.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, !outcome.equals("denied"));
                call(answer, "onResult", new Class<?>[]{Bundle.class}, result);
            }
            if (outcome.equals("replacement")) h.entry(A, AccountRetirementHost.OWNER);
            boolean allowed = outcome.equals("allowed");
            try {
                boolean removed = (boolean)call(session, "await", new Class<?>[]{answerType}, answer);
                check(allowed && removed, outcome + " reported removal without approval");
            } catch (IllegalStateException expected) {
                check(!allowed, "approved removal failed: " + expected);
                String text = expected.getMessage();
                if (outcome.equals("denied")) check(text.contains("denied"), "denial misreported as absence");
                if (outcome.equals("intent")) check(text.contains("activity"), "continuation skipped rather than requesting Activity launch");
            }
            check(h.entries.containsKey(A) != allowed, outcome + " wrong account persistence");
            check(h.access(A) != allowed, outcome + " wrong account visibility");
            check(h.queue.tasks.size() == (allowed ? 1 : 0), outcome + " wrong removal notification count");
            check("0".equals(h.durable.getProperty("hostRemoval.count")), outcome + " left an unapproved tombstone");
            check(((java.util.concurrent.ScheduledThreadPoolExecutor)AccountRetirementHost.get(h.accounts, "mSessionTimeout")).getQueue().isEmpty(), outcome + " retained auth timer");
        }
    }
    static void asynchronousCommit() throws Exception {
        try (var h = new AccountRetirementHost.H()) {
            Class<?> service = h.accounts.getClass(), auth = type("Authenticator"), answerType = type("Answer");
            Object owner = AccountRetirementHost.make(auth,
                    new Class<?>[]{String.class, ComponentName.class, int.class, int.class, int.class, int.class, boolean.class},
                    A.type, new ComponentName(AccountRetirementHost.OWNER, AccountRetirementHost.OWNER + ".Authenticator"), 0, 0, 0, 0, false);
            Object entry = h.entries.get(A);
            Object session = AccountRetirementHost.make(type("HostRemoval"),
                    new Class<?>[]{service, Account.class, entry.getClass(), auth}, h.accounts, A, entry, owner);
            Object answer = AccountRetirementHost.make(answerType,
                    new Class<?>[]{service, auth}, h.accounts, owner);
            var cleanupEntered = new java.util.concurrent.CountDownLatch(1);
            var cleanupRelease = new java.util.concurrent.CountDownLatch(1);
            AccountHostRemovalHost.retire(h, (removed, retained) -> {
                cleanupEntered.countDown();
                try { if (!cleanupRelease.await(5, java.util.concurrent.TimeUnit.SECONDS)) throw new AssertionError("cleanup release timed out"); }
                catch (InterruptedException e) { throw new AssertionError(e); }
            });
            var failure = new java.util.concurrent.atomic.AtomicReference<Throwable>();
            var result = new java.util.concurrent.atomic.AtomicReference<Boolean>();
            Thread waiter = new Thread(() -> {
                try { result.set((Boolean)call(session, "await", new Class<?>[]{answerType}, answer)); }
                catch (Throwable t) { failure.set(t); }
            }, "host-removal-waiter");
            Thread callback = new Thread(() -> {
                try {
                    Bundle approved = new Bundle(); approved.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, true);
                    call(answer, "onResult", new Class<?>[]{Bundle.class}, approved);
                } catch (Throwable t) { failure.set(t); }
            }, "authenticator-final-callback");
            waiter.start();
            try {
                long deadline = System.nanoTime() + java.util.concurrent.TimeUnit.SECONDS.toNanos(3);
                boolean listening = false;
                while (System.nanoTime() < deadline) {
                    synchronized (answer) { listening = (boolean)AccountRetirementHost.get(answer, "deliveryStarted"); }
                    if (listening) break;
                    Thread.sleep(5);
                }
                check(listening, "host did not register authenticator listener");
                callback.start();
                check(cleanupEntered.await(3, java.util.concurrent.TimeUnit.SECONDS), "async approval did not enter cleanup");
                // The durable marker invalidates current(). After its 250ms poll,
                // the waiting host must serialize close with the in-flight callback.
                Object deliveryLock = AccountRetirementHost.get(answer, "deliveryLock");
                var threads = java.lang.management.ManagementFactory.getThreadMXBean();
                deadline = System.nanoTime() + java.util.concurrent.TimeUnit.SECONDS.toNanos(3);
                boolean serialized = false;
                while (System.nanoTime() < deadline) {
                    var info = threads.getThreadInfo(waiter.threadId());
                    if (info != null && info.getThreadState() == Thread.State.BLOCKED
                            && info.getLockOwnerId() == callback.threadId()
                            && info.getLockInfo().getIdentityHashCode() == System.identityHashCode(deliveryLock)) {
                        serialized = true; break;
                    }
                    Thread.sleep(5);
                }
                check(serialized && result.get() == null, "host returned before its accepted commit completed");
            } finally {
                cleanupRelease.countDown(); callback.join(5000); waiter.join(5000);
            }
            check(!waiter.isAlive() && !callback.isAlive() && failure.get() == null, "async removal failed: " + failure.get());
            check(Boolean.TRUE.equals(result.get()) && !h.entries.containsKey(A), "host lost successful async commit result");
        }
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]); android.app.PropertyInvalidatedCache.setTestMode(true);
        for (String outcome : java.util.List.of("allowed", "denied", "error", "intent", "invalidIntent", "malformed", "replacement")) scenario(outcome);
        asynchronousCommit();
        System.out.println("Host authenticator callback/validation/commit passed: " + AccountRetirementHost.checks + " checks");
    }
}
