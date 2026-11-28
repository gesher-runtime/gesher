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
    static void accountEncoding() throws Exception {
        // Android Account permits any nonempty Java string, including unpaired
        // UTF-16 surrogates. The list must fail rather than turn one into '?'
        // and subsequently let the panel remove a different, real '?' account.
        for (String malformed : new String[]{"\uD800", "\uDC00"}) {
            for (boolean malformedType : new boolean[]{false, true}) {
                try (var h = new AccountRetirementHost.H()) {
                    h.entries.clear();
                    Account bad = malformedType ? new Account("name", malformed) : new Account(malformed, "type");
                    Account question = malformedType ? new Account("name", "?") : new Account("?", "type");
                    h.entry(bad, AccountRetirementHost.OWNER);
                    h.entry(question, AccountRetirementHost.OWNER);
                    try {
                        String[] rows = (String[])call(h.accounts, "listAccountsForHost", new Class<?>[0]);
                        throw new AssertionError("Malformed account identity was silently encoded as a different account: "
                                + java.util.Arrays.toString(rows));
                    } catch (IllegalStateException expected) {
                        check(expected.getCause() instanceof java.nio.charset.CharacterCodingException,
                                "Malformed identity did not produce a visible encoding failure");
                    }
                    check(h.entries.containsKey(bad) && h.entries.containsKey(question) && h.entries.size() == 2,
                            "Listing changed malformed or question-mark accounts");
                    check(h.queue.tasks.isEmpty(), "Failed listing emitted account removal events");
                }
            }
        }
        try (var h = new AccountRetirementHost.H()) {
            h.entries.clear();
            Account exact = new Account("\u05e9\u05dd\t\n\"?\uD83D\uDE00", "type/\t\u05d0\uD83D\uDE00");
            h.entry(exact, AccountRetirementHost.OWNER);
            String[] rows = (String[])call(h.accounts, "listAccountsForHost", new Class<?>[0]);
            String[] fields = rows[0].split("\\t", -1);
            check(rows.length == 1 && fields.length == 2, "Valid Unicode identity broke account framing");
            check(new String(java.util.Base64.getDecoder().decode(fields[0]), java.nio.charset.StandardCharsets.UTF_8).equals(exact.type)
                    && new String(java.util.Base64.getDecoder().decode(fields[1]), java.nio.charset.StandardCharsets.UTF_8).equals(exact.name),
                    "Valid Unicode identity changed during listing");
        }
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]); android.app.PropertyInvalidatedCache.setTestMode(true);
        failedMarker(); replay(false); replay(true); generationAndRace(); accountEncoding();
        System.out.println("Host account removal durability/generation/order passed: " + AccountRetirementHost.checks + " checks");
    }
}
