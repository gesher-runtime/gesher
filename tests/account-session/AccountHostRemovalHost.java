/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.accounts.Account;
import java.io.IOException;
import java.util.Set;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BiConsumer;

/** Production commit/recovery path after authenticator approval. Does not simulate
 * the authenticator UI or claim to test its approval/continuation transport. */
public final class AccountHostRemovalHost {
    static final Account A = AccountRetirementHost.REMOVED;
    static Object call(Object target, String name, Class<?>[] types, Object... args) throws Exception {
        return AccountRetirementHost.call(target, name, types, args);
    }
    static void check(boolean value, String message) { AccountRetirementHost.check(value, message); }
    static void retire(AccountRetirementHost.H h, BiConsumer<Set<Account>, Set<Account>> action) throws Exception {
        call(h.accounts, "setSyncRetirement", new Class<?>[]{BiConsumer.class}, action);
    }
    static boolean commit(AccountRetirementHost.H h, Object entry) throws Exception {
        return (boolean)call(h.accounts, "commitHostAccountRemoval",
                new Class<?>[]{Account.class, entry.getClass()}, A, entry);
    }
    static void failedMarker() throws Exception {
        try (var h = new AccountRetirementHost.H()) {
            Object entry = h.entries.get(A); var durable = h.durable;
            retire(h, (removed, retained) -> { throw new AssertionError("sync before durable marker"); });
            h.fail = true;
            try { commit(h, entry); throw new AssertionError("marker failure ignored"); }
            catch (IllegalStateException expected) { check(expected.getCause() instanceof IOException, "write failure cause"); }
            check(h.durable == durable && h.entries.get(A) != null && h.access(A), "marker failure changed visibility or durable state");
        }
    }
    static void replay(boolean finalWriteFails) throws Exception {
        try (var h = new AccountRetirementHost.H()) {
            Object entry = h.entries.get(A);
            retire(h, (removed, retained) -> {
                check(!Thread.holdsLock(h.accounts), "sync cleanup entered under account monitor");
                check(removed.equals(Set.of(A)) && retained.equals(Set.of(AccountRetirementHost.RETAINED)), "cleanup scope");
                check("1".equals(h.durable.getProperty("hostRemoval.count")), "cleanup lacks durable marker");
                if (finalWriteFails) h.fail = true;
                else throw new IllegalStateException("fixture sync persistence failure");
            });
            try { commit(h, entry); throw new AssertionError("cleanup failure ignored"); }
            catch (IllegalStateException expected) { }
            check(!h.access(A) && h.entries.containsKey(A), "failed cleanup did not fence retained account");
            check(h.queue.tasks.isEmpty(), "failed cleanup emitted removal event");
            // Restart uses exactly the protected state committed before failure.
            h.fail = false;
            var persisted = h.durable;
            try (var restarted = new AccountRetirementHost.H()) {
                restarted.entries.clear();
                call(restarted.accounts, "loadProtectedState", new Class<?>[]{java.util.Properties.class}, persisted);
                check(!restarted.access(A), "restart lost removal fence");
                retire(restarted, (removed, retained) -> check(removed.equals(Set.of(A)), "restart cleanup scope"));
                call(restarted.accounts, "reconcileHostRemovals", new Class<?>[0]);
                check(!restarted.entries.containsKey(A) && restarted.entries.containsKey(AccountRetirementHost.RETAINED), "replay did not finish exact removal");
                check("0".equals(restarted.durable.getProperty("hostRemoval.count")), "replay retained tombstone");
                check(restarted.queue.tasks.size() == 1, "replay did not enqueue removal notification");
            }
        }
    }
    static void generationAndRace() throws Exception {
        try (var h = new AccountRetirementHost.H()) {
            Object old = h.entries.get(A); h.entry(A, AccountRetirementHost.OWNER);
            check(!commit(h, old) && h.access(A), "old approval removed replacement account");
            CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
            AtomicReference<Throwable> failure = new AtomicReference<>();
            retire(h, (removed, retained) -> {
                entered.countDown();
                try { if (!release.await(5, TimeUnit.SECONDS)) throw new AssertionError("cleanup release timeout"); }
                catch (InterruptedException e) { throw new AssertionError(e); }
            });
            Object current = h.entries.get(A);
            Thread worker = new Thread(() -> { try { check(commit(h, current), "approved commit failed"); } catch (Throwable t) { failure.set(t); } });
            worker.start();
            try {
                check(entered.await(5, TimeUnit.SECONDS), "cleanup not entered");
                check(!h.access(A), "account remained accessible during cleanup");
                check(!commit(h, current), "parallel approval recommitted pending removal");
                check(h.queue.tasks.isEmpty(), "notification preceded sync cleanup");
            } finally { release.countDown(); worker.join(5000); }
            check(!worker.isAlive() && failure.get() == null, "cleanup did not complete: " + failure.get());
            check(!h.entries.containsKey(A) && h.queue.tasks.size() == 1, "cleanup/removal notification order");
        }
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]); android.app.PropertyInvalidatedCache.setTestMode(true);
        failedMarker(); replay(false); replay(true); generationAndRace();
        System.out.println("Host account removal durability/generation/order passed: " + AccountRetirementHost.checks + " checks");
    }
}
