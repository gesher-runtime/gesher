/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.Context;
import android.content.ContextWrapper;
import android.content.pm.ApplicationInfo;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.media.IAudioFocusDispatcher;
import android.media.IAudioService;
import android.os.Binder;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.RemoteException;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import static android.media.AudioManager.*;

/** Real service state/callback contracts; main also runs the generated Binder and public manager. */
public final class AudioFocusLifecycleTest {
    public static int checks;
    private static final String PKG = "gesher.audio.focus.fixture";
    // Initialized by contracts after the host/Windows entry point loads its natives.
    private static AudioAttributes MEDIA;
    private interface Work { void run() throws Exception; }
    public static void check(boolean value, String why) {
        if (!value) throw new AssertionError(why);
        checks++;
    }
    private static void rejects(Class<? extends Throwable> type, Work work, String why) throws Exception {
        try { work.run(); }
        catch (Throwable error) { check(type.isInstance(error), why + ": " + error); return; }
        throw new AssertionError(why);
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name); f.setAccessible(true); return f;
    }
    public static IAudioService service() throws Exception {
        return (IAudioService)Class.forName("com.gesher.server.AudioService")
                .getConstructor().newInstance();
    }
    private static Object lock(IAudioService service) throws Exception {
        return field(service.getClass(), "mLock").get(service);
    }
    private static int held(IAudioService service) throws Exception {
        synchronized (lock(service)) {
            return ((List<?>)field(service.getClass(), "mFocusStack").get(service)).size();
        }
    }
    private static void flush(IAudioService service) throws Exception {
        ((ExecutorService)field(service.getClass(), "mFocusCallbacks").get(service))
                .submit(() -> {}).get(5, TimeUnit.SECONDS);
    }

    /** Only Binder lifetime is controlled; every focus transition uses production AudioService. */
    public static final class Token implements InvocationHandler {
        final IBinder binder = (IBinder)Proxy.newProxyInstance(IBinder.class.getClassLoader(),
                new Class<?>[]{IBinder.class}, this);
        final List<IBinder.DeathRecipient> recipients = new ArrayList<>();
        boolean dead, rejectLink, dieDuringLink, failAlive;
        int links, unlinks;
        @Override public synchronized Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
            switch (method.getName()) {
                case "hashCode": return System.identityHashCode(proxy);
                case "equals": return proxy == args[0];
                case "toString": return "AudioFocusToken@" + System.identityHashCode(proxy);
                case "pingBinder": return !dead;
                case "isBinderAlive": return !dead && !failAlive;
                case "queryLocalInterface": return null;
                case "linkToDeath":
                    links++;
                    if (dead || rejectLink) throw new RemoteException("fixture registration rejected");
                    IBinder.DeathRecipient recipient = (IBinder.DeathRecipient)args[0];
                    recipients.add(recipient);
                    if (dieDuringLink) { dead = true; recipient.binderDied(); }
                    return null;
                case "unlinkToDeath": unlinks++; return recipients.remove(args[0]);
                default: throw new UnsupportedOperationException("Unexpected fixture Binder call " + method);
            }
        }
        synchronized int linked() { return recipients.size(); }
        synchronized IBinder.DeathRecipient recipient() { return recipients.get(0); }
        void die() {
            List<IBinder.DeathRecipient> pending;
            synchronized (this) { dead = true; pending = new ArrayList<>(recipients); }
            for (IBinder.DeathRecipient recipient : pending) recipient.binderDied();
        }
    }

    private static class Callback implements IAudioFocusDispatcher {
        final BlockingQueue<String> events = new LinkedBlockingQueue<>();
        final Token token = new Token();
        final String client;
        private final Object serviceLock;
        private final Thread caller = Thread.currentThread();
        volatile boolean underLock, inline;
        Callback(IAudioService service, String client) throws Exception {
            serviceLock = lock(service); this.client = client;
        }
        @Override public IBinder asBinder() { return token.binder; }
        @Override public void dispatchAudioFocusChange(int change, String id) throws RemoteException {
            underLock |= Thread.holdsLock(serviceLock);
            inline |= Thread.currentThread() == caller;
            events.add(id + ":" + change);
        }
        @Override public void dispatchFocusResultFromExtPolicy(int result, String id) {
            throw new AssertionError("Ordinary stack invented an external-policy result");
        }
        void event(int expected) throws Exception {
            String actual = events.poll(5, TimeUnit.SECONDS);
            check((client + ":" + expected).equals(actual), "Wrong/missing event for " + client + ": " + actual);
            check(!underLock && !inline, "Listener ran under the service lock or on the caller thread");
        }
        void empty(IAudioService service) throws Exception {
            flush(service); check(events.isEmpty(), "Unexpected callback for " + client + ": " + events);
        }
    }

    private static int request(IAudioService service, String id, int gain, Token token,
            Callback callback, int flags) throws Exception {
        return service.requestAudioFocus(MEDIA, gain, token.binder, callback, id, PKG, null, flags, null, 36);
    }
    private static void grant(IAudioService service, String id, int gain, Token token,
            Callback callback) throws Exception {
        check(request(service, id, gain, token, callback, 0) == AUDIOFOCUS_REQUEST_GRANTED, "Focus request denied: " + id);
    }
    private static void abandon(IAudioService service, String id) throws Exception {
        check(service.abandonAudioFocus(null, id, MEDIA, PKG) == AUDIOFOCUS_REQUEST_GRANTED, "Abandon failed: " + id);
    }

    private static void rejectedRequests(IAudioService service) throws Exception {
        Token owner = new Token(); Callback active = new Callback(service, "active");
        grant(service, "active", AUDIOFOCUS_GAIN, owner, active);
        Token broken = new Token(); broken.rejectLink = true;
        check(request(service, "failed", AUDIOFOCUS_GAIN, broken, null, 0) == AUDIOFOCUS_REQUEST_FAILED,
                "Failed link was reported granted");
        // This is deliberately before flush: the old source has no callback executor.
        check(service.getCurrentAudioFocus() == AUDIOFOCUS_GAIN && held(service) == 1 && owner.linked() == 1,
                "Failed new request changed the existing focus owner");
        active.empty(service);
        check(broken.linked() == 0, "Rejected link retained a registration");
        check(request(service, "active", AUDIOFOCUS_GAIN, broken, null, 0) == AUDIOFOCUS_REQUEST_FAILED,
                "Unchanged top bypassed lifetime registration failure");
        check(owner.linked() == 1 && held(service) == 1, "Failed same-client request retired the owner");
        for (int kind = 0; kind < 3; kind++) {
            Token candidate = new Token();
            candidate.dead = kind == 0; candidate.dieDuringLink = kind == 1; candidate.failAlive = kind == 2;
            check(request(service, "dead-" + kind, AUDIOFOCUS_GAIN, candidate, null, 0) == AUDIOFOCUS_REQUEST_FAILED,
                    "Dead candidate was reported granted: " + kind);
            check(held(service) == 1 && owner.linked() == 1 && candidate.linked() == 0,
                    "Dead registration changed state or retained a recipient: " + kind);
            active.empty(service);
        }
        Token invalid = new Token();
        check(request(service, "lock", AUDIOFOCUS_GAIN, invalid, null, AUDIOFOCUS_FLAG_LOCK) == AUDIOFOCUS_REQUEST_FAILED,
                "Unsupported focus lock was granted");
        check(request(service, "flags", AUDIOFOCUS_GAIN, invalid, null, 0x1000) == AUDIOFOCUS_REQUEST_FAILED,
                "Unknown flags were granted");
        rejects(IllegalArgumentException.class,
                () -> request(service, "test", AUDIOFOCUS_GAIN, invalid, null, AUDIOFOCUS_FLAG_TEST),
                "Test flag accepted on ordinary Binder API");
        for (int gain : new int[]{AUDIOFOCUS_NONE, -1, 5}) {
            check(request(service, "invalid", gain, invalid, null, 0) == AUDIOFOCUS_REQUEST_FAILED,
                    "Invalid focus gain was granted");
        }
        check(service.requestAudioFocus(null, 1, invalid.binder, null, "null-aa", PKG, null, 0, null, 36) == 0,
                "Null attributes accepted");
        check(service.requestAudioFocus(MEDIA, 1, null, null, "null-token", PKG, null, 0, null, 36) == 0,
                "Null lifetime token accepted");
        check(service.requestAudioFocus(MEDIA, 1, invalid.binder, null, null, PKG, null, 0, null, 36) == 0,
                "Null client accepted");
        check(service.requestAudioFocus(MEDIA, 1, invalid.binder, null, "null-package", null, null, 0, null, 36) == 0,
                "Null package accepted");
        check(invalid.links == 0 && held(service) == 1 && service.getCurrentAudioFocus() == AUDIOFOCUS_GAIN,
                "Rejected input registered a token or changed active focus");
        active.empty(service); abandon(service, "active");
        check(owner.linked() == 0 && held(service) == 0, "Owner abandon retained state");
    }

    private static void lossEscalation(IAudioService service) throws Exception {
        Token ta = new Token(), tb = new Token(), tc = new Token(), td = new Token(), te = new Token();
        Callback a = new Callback(service, "a"), b = new Callback(service, "b"), c = new Callback(service, "c"),
                d = new Callback(service, "d"), e = new Callback(service, "e");
        grant(service, "a", AUDIOFOCUS_GAIN_TRANSIENT, ta, a); a.empty(service);
        grant(service, "b", AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK, tb, b); a.event(AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK);
        grant(service, "c", AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK, tc, c); b.event(AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK);
        a.empty(service);
        grant(service, "d", AUDIOFOCUS_GAIN_TRANSIENT_EXCLUSIVE, td, d);
        a.event(AUDIOFOCUS_LOSS_TRANSIENT); b.event(AUDIOFOCUS_LOSS_TRANSIENT); c.event(AUDIOFOCUS_LOSS_TRANSIENT);
        grant(service, "e", AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK, te, e);
        d.event(AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK); a.empty(service); b.empty(service); c.empty(service);
        check(held(service) == 5, "Transient loss removed a waiter");
        abandon(service, "e"); d.event(AUDIOFOCUS_GAIN);
        check(service.getCurrentAudioFocus() == AUDIOFOCUS_GAIN_TRANSIENT_EXCLUSIVE, "Current focus lost the requested gain type");
        abandon(service, "d"); c.event(AUDIOFOCUS_GAIN);
        abandon(service, "c"); b.event(AUDIOFOCUS_GAIN);
        abandon(service, "b"); a.event(AUDIOFOCUS_GAIN);
        abandon(service, "a");
        check(held(service) == 0 && ta.linked() + tb.linked() + tc.linked() + td.linked() + te.linked() == 0,
                "Transient unwind retained registrations");
        grant(service, "a", AUDIOFOCUS_GAIN, ta, a);
        grant(service, "b", AUDIOFOCUS_GAIN_TRANSIENT, tb, b); a.event(AUDIOFOCUS_LOSS_TRANSIENT);
        grant(service, "c", AUDIOFOCUS_GAIN, tc, c); a.event(AUDIOFOCUS_LOSS); b.event(AUDIOFOCUS_LOSS);
        check(held(service) == 1 && ta.linked() == 0 && tb.linked() == 0, "Permanent loss left previous owners registered");
        abandon(service, "c"); a.empty(service); b.empty(service);
        check(service.getCurrentAudioFocus() == AUDIOFOCUS_NONE, "Permanent loser regained after winner abandoned");
    }

    private static void sameClient(IAudioService service) throws Exception {
        Token lowerToken = new Token(), old = new Token(), candidate = new Token(), replacement = new Token();
        Callback lower = new Callback(service, "lower"), original = new Callback(service, "same"),
                ignored = new Callback(service, "same"), newer = new Callback(service, "same");
        grant(service, "lower", AUDIOFOCUS_GAIN, lowerToken, lower);
        grant(service, "same", AUDIOFOCUS_GAIN_TRANSIENT, old, original); lower.event(AUDIOFOCUS_LOSS_TRANSIENT);
        IBinder.DeathRecipient stale = old.recipient();
        grant(service, "same", AUDIOFOCUS_GAIN_TRANSIENT, candidate, ignored);
        check(candidate.links == 1 && candidate.linked() == 0 && old.linked() == 1 && held(service) == 2,
                "Unchanged top replaced its original token or retained candidate registration");
        candidate.die(); check(held(service) == 2, "Discarded no-op candidate retired current owner");
        Token intruder = new Token(); Callback other = new Callback(service, "other");
        grant(service, "other", AUDIOFOCUS_GAIN_TRANSIENT, intruder, other); original.event(AUDIOFOCUS_LOSS_TRANSIENT);
        ignored.empty(service); abandon(service, "other"); original.event(AUDIOFOCUS_GAIN);
        grant(service, "same", AUDIOFOCUS_GAIN, replacement, newer); lower.event(AUDIOFOCUS_LOSS);
        check(old.linked() == 0 && lowerToken.linked() == 0 && replacement.linked() == 1 && held(service) == 1,
                "Changed gain did not replace the full record and permanently retire lower owner");
        stale.binderDied(); check(held(service) == 1 && replacement.linked() == 1, "Old token death removed replacement by client ID");
        original.empty(service); newer.empty(service);
        grant(service, "other", AUDIOFOCUS_GAIN_TRANSIENT, intruder, other); newer.event(AUDIOFOCUS_LOSS_TRANSIENT);
        abandon(service, "other"); newer.event(AUDIOFOCUS_GAIN);
        Token flagsToken = new Token(); Callback flagsListener = new Callback(service, "same");
        IBinder.DeathRecipient staleReplacement = replacement.recipient();
        check(request(service, "same", AUDIOFOCUS_GAIN, flagsToken, flagsListener,
                AUDIOFOCUS_FLAG_DELAY_OK | AUDIOFOCUS_FLAG_PAUSES_ON_DUCKABLE_LOSS) == AUDIOFOCUS_REQUEST_GRANTED,
                "Ordinary delay/pause flags were rejected without a locked owner");
        check(replacement.linked() == 0 && flagsToken.linked() == 1 && held(service) == 1,
                "Changed flags did not replace token/dispatcher");
        staleReplacement.binderDied(); check(held(service) == 1, "Retired flag-generation removed current owner");
        grant(service, "other", AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK, intruder, other);
        flagsListener.event(AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK); newer.empty(service);
        abandon(service, "other"); flagsListener.event(AUDIOFOCUS_GAIN); abandon(service, "same");

        grant(service, "same", AUDIOFOCUS_GAIN, old, original);
        grant(service, "other", AUDIOFOCUS_GAIN_TRANSIENT, intruder, other); original.event(AUDIOFOCUS_LOSS_TRANSIENT);
        grant(service, "same", AUDIOFOCUS_GAIN, replacement, newer); other.event(AUDIOFOCUS_LOSS);
        check(old.linked() == 0 && replacement.linked() == 1 && intruder.linked() == 0 && held(service) == 1,
                "Lower same-client reacquisition duplicated its registration or restored the old owner");
        newer.empty(service); original.empty(service); abandon(service, "same");
    }

    private static void tokenDeathAndRemoval(IAudioService service) throws Exception {
        Token survivor = new Token(), shared = new Token();
        Callback a = new Callback(service, "survivor"), b = new Callback(service, "sibling-one"),
                c = new Callback(service, "sibling-two");
        grant(service, "survivor", AUDIOFOCUS_GAIN, survivor, a);
        grant(service, "sibling-one", AUDIOFOCUS_GAIN_TRANSIENT, shared, b); a.event(AUDIOFOCUS_LOSS_TRANSIENT);
        grant(service, "sibling-two", AUDIOFOCUS_GAIN_TRANSIENT, shared, c); b.event(AUDIOFOCUS_LOSS_TRANSIENT);
        check(shared.linked() == 2, "One lifetime token did not retain both listeners");
        shared.die(); a.event(AUDIOFOCUS_GAIN); b.empty(service); c.empty(service);
        check(held(service) == 1 && shared.linked() == 0, "Shared death retained a sibling or restored it before removing all");
        a.empty(service); abandon(service, "survivor");

        Token reused = new Token(); Callback first = new Callback(service, "reuse"), second = new Callback(service, "reuse");
        grant(service, "reuse", AUDIOFOCUS_GAIN, reused, first);
        IBinder.DeathRecipient stale = reused.recipient(); abandon(service, "reuse");
        grant(service, "reuse", AUDIOFOCUS_GAIN_TRANSIENT, reused, second);
        stale.binderDied(); check(held(service) == 1 && reused.linked() == 1, "Stale same-token registration retired a new generation");
        reused.die(); check(held(service) == 0 && reused.linked() == 0, "Active registration death failed cleanup");

        Token lower = new Token(), middle = new Token(), top = new Token();
        Callback l = new Callback(service, "lower"), m = new Callback(service, "middle"), t = new Callback(service, "top");
        grant(service, "lower", AUDIOFOCUS_GAIN, lower, l);
        grant(service, "middle", AUDIOFOCUS_GAIN_TRANSIENT, middle, m); l.event(AUDIOFOCUS_LOSS_TRANSIENT);
        grant(service, "top", AUDIOFOCUS_GAIN_TRANSIENT, top, t); m.event(AUDIOFOCUS_LOSS_TRANSIENT);
        middle.die(); check(held(service) == 2 && top.linked() == 1, "Non-top death displaced the focus holder");
        t.empty(service); l.empty(service);
        service.unregisterAudioFocusClient("top"); l.empty(service);
        check(held(service) == 1 && top.linked() == 0, "Unregister did not remove current owner");
        abandon(service, "missing"); l.empty(service);
        grant(service, "top", AUDIOFOCUS_GAIN_TRANSIENT, top, t); l.empty(service);
        top.die(); l.event(AUDIOFOCUS_GAIN); abandon(service, "lower");
        check(held(service) == 0, "Removal cases retained state");
    }

    private static void callbacks(IAudioService service) throws Exception {
        Token heldToken = new Token(), transientToken = new Token();
        AtomicInteger failures = new AtomicInteger();
        Callback broken = new Callback(service, "broken") {
            @Override public void dispatchAudioFocusChange(int change, String id) throws RemoteException {
                if (failures.incrementAndGet() == 1) throw new RemoteException("remote callback gone");
                throw new IllegalStateException("local callback rejected delivery");
            }
        };
        grant(service, "broken", AUDIOFOCUS_GAIN, heldToken, broken);
        grant(service, "temporary", AUDIOFOCUS_GAIN_TRANSIENT, transientToken, null); flush(service);
        check(failures.get() == 1 && held(service) == 2, "Failed listener interrupted loss state");
        abandon(service, "temporary"); flush(service);
        check(failures.get() == 2 && held(service) == 1, "Failed listener interrupted restoration");
        abandon(service, "broken");

        ExecutorService worker = Executors.newSingleThreadExecutor();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        CountDownLatch reentered = new CountDownLatch(1);
        Callback reentrant = new Callback(service, "reentrant") {
            @Override public void dispatchAudioFocusChange(int change, String id) throws RemoteException {
                super.dispatchAudioFocusChange(change, id);
                if (change == AUDIOFOCUS_LOSS_TRANSIENT) {
                    try {
                        worker.submit(() -> {
                            try { service.abandonAudioFocus(null, "temporary", MEDIA, PKG); }
                            catch (RemoteException error) { throw new RuntimeException(error); }
                        }).get(5, TimeUnit.SECONDS);
                    } catch (Throwable error) { failure.set(error); }
                    finally { reentered.countDown(); }
                }
            }
        };
        try {
            grant(service, "reentrant", AUDIOFOCUS_GAIN, heldToken, reentrant);
            grant(service, "temporary", AUDIOFOCUS_GAIN_TRANSIENT, transientToken, null);
            check(reentered.await(6, TimeUnit.SECONDS) && failure.get() == null,
                    "Cross-thread callback reentry blocked or failed: " + failure.get());
            reentrant.event(AUDIOFOCUS_LOSS_TRANSIENT); reentrant.event(AUDIOFOCUS_GAIN);
            check(held(service) == 1 && service.getCurrentAudioFocus() == AUDIOFOCUS_GAIN,
                    "Reentrant abandon did not update actual focus state");
            abandon(service, "reentrant");
        } finally { worker.shutdownNow(); }

        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        Callback ordered = new Callback(service, "ordered") {
            @Override public void dispatchAudioFocusChange(int change, String id) throws RemoteException {
                super.dispatchAudioFocusChange(change, id);
                if (change == AUDIOFOCUS_LOSS_TRANSIENT) {
                    entered.countDown();
                    try { if (!release.await(5, TimeUnit.SECONDS)) failure.set(new AssertionError("Blocked listener not released")); }
                    catch (InterruptedException error) { Thread.currentThread().interrupt(); failure.set(error); }
                }
            }
        };
        Token winner = new Token();
        grant(service, "ordered", AUDIOFOCUS_GAIN, heldToken, ordered);
        try {
            grant(service, "temporary", AUDIOFOCUS_GAIN_TRANSIENT, transientToken, null);
            check(entered.await(5, TimeUnit.SECONDS), "Queued loss did not start");
            abandon(service, "temporary"); grant(service, "winner", AUDIOFOCUS_GAIN, winner, null);
            check(held(service) == 1 && heldToken.linked() == 0, "Blocked callback prevented later state commits");
        } finally { release.countDown(); }
        ordered.event(AUDIOFOCUS_LOSS_TRANSIENT); ordered.event(AUDIOFOCUS_GAIN); ordered.event(AUDIOFOCUS_LOSS);
        check(failure.get() == null, "Ordered callback failed: " + failure.get());
        abandon(service, "winner");
    }

    private static void stackBound(IAudioService service) throws Exception {
        List<Token> tokens = new ArrayList<>();
        boolean granted = true;
        // Pinned MediaFocusControl checks size > 100 before registration, allowing 101.
        for (int i = 0; i <= 100; i++) {
            Token token = new Token(); tokens.add(token);
            granted &= request(service, "bound-" + i, AUDIOFOCUS_GAIN_TRANSIENT, token, null, 0) == AUDIOFOCUS_REQUEST_GRANTED;
        }
        check(granted && held(service) == 101, "Pinned focus-stack bound rejected an allowed entry");
        Token overflow = new Token();
        check(request(service, "overflow", AUDIOFOCUS_GAIN_TRANSIENT, overflow, null, 0) == AUDIOFOCUS_REQUEST_FAILED,
                "Focus stack grew beyond its bound");
        check(request(service, "bound-100", AUDIOFOCUS_GAIN_TRANSIENT, overflow, null, 0) == AUDIOFOCUS_REQUEST_FAILED,
                "Full-stack same-client request bypassed the pinned entry check");
        check(overflow.links == 0 && held(service) == 101, "Stack overflow registered a token or mutated state");
        for (int i = 0; i <= 100; i++) service.unregisterAudioFocusClient("bound-" + i);
        boolean unlinked = true;
        for (Token token : tokens) unlinked &= token.linked() == 0;
        check(unlinked && held(service) == 0, "Bounded stack cleanup retained lifetime registrations");
    }

    public static void contracts(IAudioService service) throws Exception {
        MEDIA = new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build();
        rejectedRequests(service); lossEscalation(service); sameClient(service);
        tokenDeathAndRemoval(service); callbacks(service); stackBound(service);
        check(held(service) == 0 && service.getCurrentAudioFocus() == AUDIOFOCUS_NONE, "Fixture left an active focus owner");
    }

    private static IAudioService wire(IAudioService service) throws Exception {
        IBinder target = (IBinder)Class.forName("com.gesher.server.stub.IAudioServiceStub")
                .getConstructor(IAudioService.class).newInstance(service);
        return IAudioService.Stub.asInterface(new Binder() {
            @Override protected boolean onTransact(int code, Parcel request, Parcel reply, int flags) throws RemoteException {
                return target.transact(code, request, reply, flags);
            }
        });
    }
    private static void publicManager(IAudioService service) throws Exception {
        IAudioService binder = wire(service);
        Field serviceField = field(AudioManager.class, "sService"); Object original = serviceField.get(null);
        HandlerThread thread = new HandlerThread("audio-focus-fixture-listener"); thread.start();
        try {
            serviceField.set(null, binder);
            AudioManager manager = new AudioManager();
            // Use the real framework manager's test constructor without initializing unrelated
            // platform routing. Its focus path still uses Context, Binder, listener map and Handler.
            field(AudioManager.class, "mOriginalContext").set(manager, new ContextWrapper(null) {
                @Override public int getDeviceId() { return Context.DEVICE_ID_DEFAULT; }
                @Override public Context getApplicationContext() { return this; }
                @Override public String getOpPackageName() { return PKG; }
                @Override public String getAttributionTag() { return null; }
                @Override public ApplicationInfo getApplicationInfo() {
                    ApplicationInfo info = new ApplicationInfo(); info.targetSdkVersion = 36; return info;
                }
            });
            Handler handler = new Handler(thread.getLooper());
            BlockingQueue<Integer> firstEvents = new LinkedBlockingQueue<>(), secondEvents = new LinkedBlockingQueue<>();
            AudioManager.OnAudioFocusChangeListener first = firstEvents::add, second = secondEvents::add;
            AudioFocusRequest firstRequest = new AudioFocusRequest.Builder(AUDIOFOCUS_GAIN_TRANSIENT)
                    .setAudioAttributes(MEDIA).setOnAudioFocusChangeListener(first, handler).build();
            AudioFocusRequest secondRequest = new AudioFocusRequest.Builder(AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK)
                    .setAudioAttributes(MEDIA).setOnAudioFocusChangeListener(second, handler).build();
            check(manager.requestAudioFocus(firstRequest) == AUDIOFOCUS_REQUEST_GRANTED, "Public first request failed");
            check(manager.requestAudioFocus(secondRequest) == AUDIOFOCUS_REQUEST_GRANTED, "Public competing request failed");
            check(Integer.valueOf(AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK).equals(firstEvents.poll(5, TimeUnit.SECONDS)),
                    "Public framework listener lost duck notification");
            check(manager.abandonAudioFocusRequest(secondRequest) == AUDIOFOCUS_REQUEST_GRANTED, "Public abandon failed");
            check(Integer.valueOf(AUDIOFOCUS_GAIN).equals(firstEvents.poll(5, TimeUnit.SECONDS)),
                    "Public transient owner did not regain with GAIN");
            check(manager.abandonAudioFocusRequest(firstRequest) == AUDIOFOCUS_REQUEST_GRANTED, "Public final abandon failed");
            check(binder.getCurrentAudioFocus() == AUDIOFOCUS_NONE && held(service) == 0 && secondEvents.isEmpty(),
                    "Public Binder/manager path retained focus or invented an initial gain callback");
            check(binder.requestAudioFocus(MEDIA, AUDIOFOCUS_GAIN, new Binder(), null, "locked", PKG,
                    null, AUDIOFOCUS_FLAG_LOCK, null, 36) == AUDIOFOCUS_REQUEST_FAILED,
                    "Generated Binder stub granted unsupported locked focus");
            rejects(IllegalArgumentException.class, () -> new AudioFocusRequest.Builder(0),
                    "Public manager request builder accepted invalid gain");
        } finally {
            serviceField.set(null, original); thread.quitSafely(); thread.join(5000);
        }
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime"); Looper.prepare();
        try {
            IAudioService service = service(); contracts(service); publicManager(service);
            System.out.println("AudioFocusLifecycleTest all passed: " + checks + " checks");
        } finally { Looper.myLooper().quit(); }
    }
}
