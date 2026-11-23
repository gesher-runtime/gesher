/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.Manifest;
import android.app.ActivityManager;
import android.app.AppOpsManager;
import android.app.SyncNotedAppOp;
import android.content.AttributionSource;
import android.content.AttributionSourceState;
import android.content.pm.PackageManager;
import android.content.pm.ParceledListSlice;
import android.os.Binder;
import android.os.IBinder;
import android.os.Process;
import android.os.RemoteException;
import android.os.SystemClock;
import android.os.UserHandle;
import android.util.LongSparseArray;
import com.android.internal.app.IAppOpsCallback;
import com.android.internal.app.IAppOpsService;
import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Supplier;
import static android.app.AppOpsManager.*;
import static android.app.AppOpsManager.OnOpStartedListener.*;
import static com.gesher.server.AppOpsModeStore.*;

/**
 * Android AppOps modes and attributed operation lifetimes. Policy follows Android 16 r4
 * AppOpsService/PermissionCheckerService; Windows supplies process state, not alternate modes.
 * Disk writes and Binder calls are never made while holding the operation-state monitor.
 */
public final class AppOpsService extends IAppOpsService.Default {
    private final PackageManagerService mPm;
    private final AppOpsModeStore mStore;
    private final Object mMutationLock = new Object();
    private final Object mLock = new Object();
    private final Object mClientRegistration = new Object();
    private final Object mUidRefreshLock = new Object();
    private final ScheduledThreadPoolExecutor mEvents = new ScheduledThreadPoolExecutor(1, r -> {
        Thread t = new Thread(r, "gesher-appops"); t.setDaemon(true); return t;
    });
    private final AtomicBoolean mRefreshQueued = new AtomicBoolean();
    private volatile CallerIdentity mCallers;
    private volatile Supplier<Map<Integer, UidState>> mStateSource = Collections::emptyMap;
    private final Map<Integer, UidState> mUidStates = new HashMap<>();
    private final Map<Integer, PendingState> mPendingStates = new HashMap<>();
    private final Map<Integer, java.util.concurrent.ScheduledFuture<?>> mStateTimers = new HashMap<>();
    private final Map<IBinder, Client> mClients = new HashMap<>();
    private final Map<OpKey, History> mHistory = new HashMap<>();
    private final Map<IBinder, ModeWatcher> mWatchers = new HashMap<>();
    private final CopyOnWriteArrayList<ModeChangedListener> mModeListeners = new CopyOnWriteArrayList<>();

    public interface ModeChangedListener {
        void onModeChanged(ApkPackage current, int op, int oldMode, int newMode);
    }
    public AutoCloseable addModeChangedListener(ModeChangedListener listener) {
        mModeListeners.add(Objects.requireNonNull(listener));
        return () -> mModeListeners.remove(listener);
    }

    AppOpsService(PackageManagerService pm, File file) {
        mPm = Objects.requireNonNull(pm);
        mStore = new AppOpsModeStore(file);
        mEvents.setRemoveOnCancelPolicy(true);
        pm.onPackageRemoval(this::packageRemoved);
        pm.onPackageChange(this::packageChanged);
        pm.permissions().addRuntimeModeListener(this::permissionModesChanged);
    }

    void attach(CallerIdentity callers, Supplier<Map<Integer, UidState>> source) {
        mCallers = Objects.requireNonNull(callers);
        mStateSource = Objects.requireNonNull(source);
        requestUidStateRefresh();
    }

    static void validateOp(int op) {
        if (op < 0 || op >= _NUM_OP) throw new IllegalArgumentException("Unknown app op " + op);
    }
    static void validateMode(int mode) {
        if (mode < MODE_ALLOWED || mode > MODE_FOREGROUND) throw new IllegalArgumentException("Unknown app op mode " + mode);
    }
    private static int switchOp(int op) { validateOp(op); return AppOpsManager.opToSwitch(op); }
    private CallerIdentity callers() {
        CallerIdentity callers = mCallers;
        if (callers == null) throw new SecurityException("AppOps identity service is not attached");
        return callers;
    }
    private boolean hasPermission(String permission) {
        CallerIdentity callers = callers();
        return callers.isSystem() || mPm.permissions().checkProcessPermission(permission,
                callers.callerPid(), callers.callerUid()) == PackageManager.PERMISSION_GRANTED;
    }
    private void enforce(String permission) {
        if (!hasPermission(permission)) throw new SecurityException("AppOps requires " + permission);
    }
    private void verifyIncomingUid(int uid) {
        CallerIdentity callers = callers();
        if (callers.isSystem()) return;
        String name = callers.callerPackage();
        ApkPackage pkg = mPm.get(name);
        if (pkg != null && pkg.uid() == uid && callers.callerUid() == uid) return;
        enforce(Manifest.permission.UPDATE_APP_OPS_STATS);
    }
    private void verifyReadableOp(int op) {
        validateOp(op);
        if (AppOpsManager.opRestrictsRead(op)
                && !hasPermission(Manifest.permission.MANAGE_APPOPS)
                && !hasPermission(Manifest.permission.GET_APP_OPS_STATS)) {
            enforce(Manifest.permission.MANAGE_APP_OPS_MODES);
        }
    }
    private ApkPackage packageFor(int uid, String name) {
        ApkPackage pkg = name == null ? null : mPm.get(name);
        return pkg != null && pkg.uid() == uid && !mPm.isRemovalPending(name) ? pkg : null;
    }
    private boolean platformPackage(int uid, String name) {
        return "android".equals(name) && (uid == Process.SYSTEM_UID || uid == Process.ROOT_UID);
    }
    @Override public int checkPackage(int uid, String packageName) {
        Objects.requireNonNull(packageName);
        return packageFor(uid, packageName) != null || platformPackage(uid, packageName) ? MODE_ALLOWED : MODE_ERRORED;
    }
    @Override public int permissionToOpCode(String permission) {
        return AppOpsManager.permissionToOpCode(permission);
    }
    private String attribution(ApkPackage pkg, String tag) {
        if (tag == null || pkg == null) return null;
        for (var declared : pkg.parsed.getAttributions()) if (tag.equals(declared.getTag())) return tag;
        // AOSP discards an undeclared tag; it never creates arbitrary named attribution state.
        return null;
    }
    private boolean validOwner(Owner owner) {
        return !mPm.isRemovalPending(owner.name()) && owner.matches(mPm.get(owner.name()));
    }
    private int raw(State state, int op, int uid, String name) {
        Integer runtime = mPm.permissions().runtimeOpMode(op, uid);
        if (runtime != null) return runtime;
        int initial = AppOpsManager.opToDefaultMode(op);
        UidMode uidMode = state.uids().get(new UidOp(uid, op));
        if (uidMode != null && uidMode.mode() != initial
                && uidMode.owners().stream().anyMatch(this::validOwner)) return uidMode.mode();
        PackageMode pkgMode = state.packages().get(new PackageOp(uid, name, op));
        return pkgMode != null && validOwner(pkgMode.owner()) ? pkgMode.mode() : initial;
    }
    /** Server-local raw policy for special-access consumers such as AlarmManager. */
    public int checkRawMode(int code, int uid, String name) {
        int op = switchOp(code);
        if (platformPackage(uid, name)) return MODE_ALLOWED;
        if (packageFor(uid, name) == null) return MODE_ERRORED;
        return raw(mStore.state(), op, uid, name);
    }
    @Override public int checkOperationRaw(int code, int uid, String name, String tag) {
        return checkOperationRawForDevice(code, uid, name, tag, 0);
    }
    @Override public int checkOperationRawForDevice(int code, int uid, String name, String tag, int device) {
        verifyReadableOp(code);
        if (device != 0) return MODE_IGNORED;
        return restricted(code, uid, name, tag, true) ? MODE_IGNORED : checkRawMode(code, uid, name);
    }
    @Override public int checkOperation(int code, int uid, String name) {
        return checkOperationForDevice(code, uid, name, null, 0);
    }
    @Override public int checkOperationForDevice(int code, int uid, String name, String tag, int device) {
        verifyReadableOp(code);
        if (device != 0) return MODE_IGNORED;
        int mode = restricted(code, uid, name, null, true) ? MODE_IGNORED : checkRawMode(code, uid, name);
        // Only MODE_FOREGROUND needs process/activity/service state. This API
        // records no access history, unlike note/start, so an ordinary fixed-mode
        // check need not rebuild every UID's state under the task/service locks.
        if (mode != MODE_FOREGROUND) return mode;
        refreshUidStates();
        return effective(code, uid, name);
    }
    private int effective(int code, int uid, String name) {
        int mode = restricted(code, uid, name, null, true) ? MODE_IGNORED : checkRawMode(code, uid, name);
        synchronized (mLock) { return evaluate(code, uid, mode); }
    }

    @Override public void setMode(int code, int uid, String name, int mode) {
        enforce(Manifest.permission.MANAGE_APP_OPS_MODES);
        int op = switchOp(code); validateMode(mode);
        // Android 16's checking service ignores package modes for runtime permission ops.
        if (mPm.permissions().runtimePermissionForOp(op) != null) return;
        List<Change> changes;
        synchronized (mMutationLock) {
            synchronized (mPm) {
                ApkPackage pkg = packageFor(uid, name);
                if (pkg == null) return;
                State before = mStore.state();
                Map<PackageOp, PackageMode> packages = new HashMap<>(before.packages());
                PackageOp key = new PackageOp(uid, name, op);
                if (mode == AppOpsManager.opToDefaultMode(op)) packages.remove(key);
                else packages.put(key, new PackageMode(mode, mStore.owner(pkg)));
                State after = new State(before.uids(), packages);
                changes = changes(before, after, List.of(pkg), Set.of(op));
                if (!before.equals(after)) mStore.commit(after);
            }
        }
        changed(changes);
    }
    @Override public void setUidMode(int code, int uid, int mode) {
        enforce(Manifest.permission.MANAGE_APP_OPS_MODES);
        setUidModeChecked(code, uid, null, mode);
    }
    /** Commits only while this exact installed package generation still owns the UID. */
    public boolean setUidModeIfCurrent(int code, ApkPackage expected, int mode) {
        enforce(Manifest.permission.MANAGE_APP_OPS_MODES);
        Objects.requireNonNull(expected);
        return setUidModeChecked(code, expected.uid(), expected, mode);
    }
    private boolean setUidModeChecked(int code, int uid, ApkPackage expected, int mode) {
        int op = switchOp(code); validateMode(mode);
        if (uid < 0) throw new IllegalArgumentException("Invalid UID");
        if (mPm.permissions().runtimePermissionForOp(op) != null) {
            return mPm.permissions().setRuntimeAppOpMode(uid, expected, op, mode);
        }
        List<Change> changes;
        synchronized (mMutationLock) {
            synchronized (mPm) {
                if (expected != null && packageFor(uid, expected.packageName) != expected) return false;
                List<ApkPackage> packages = packagesForUid(uid);
                if (packages.isEmpty()) return false;
                State before = mStore.state();
                Map<UidOp, UidMode> uids = new HashMap<>(before.uids());
                UidOp key = new UidOp(uid, op);
                if (mode == AppOpsManager.opToDefaultMode(op)) uids.remove(key);
                else {
                    List<Owner> owners = new ArrayList<>();
                    for (ApkPackage pkg : packages) owners.add(mStore.owner(pkg));
                    uids.put(key, new UidMode(mode, owners));
                }
                State after = new State(uids, before.packages());
                changes = changes(before, after, packages, Set.of(op));
                if (!before.equals(after)) mStore.commit(after);
            }
        }
        changed(changes);
        return true;
    }
    private List<ApkPackage> packagesForUid(int uid) {
        List<ApkPackage> packages = new ArrayList<>();
        for (ApkPackage pkg : mPm.packages()) if (pkg.uid() == uid && !mPm.isRemovalPending(pkg.packageName)) packages.add(pkg);
        return packages;
    }
    private record Change(ApkPackage pkg, int op, int oldMode, int newMode) { }
    private List<Change> changes(State before, State after, List<ApkPackage> packages, Set<Integer> ops) {
        List<Change> changes = new ArrayList<>();
        for (ApkPackage pkg : packages) for (int op : ops) {
            int oldMode = raw(before, op, pkg.uid(), pkg.packageName);
            int newMode = raw(after, op, pkg.uid(), pkg.packageName);
            if (oldMode != newMode) changes.add(new Change(pkg, op, oldMode, newMode));
        }
        return changes;
    }
    private void changed(List<Change> changes) {
        refreshRunningOperations();
        for (Change change : changes) {
            if (packageFor(change.pkg.uid(), change.pkg.packageName) != change.pkg) continue;
            long identity = Binder.clearCallingIdentity();
            try {
                for (ModeChangedListener listener : mModeListeners) {
                    if (packageFor(change.pkg.uid(), change.pkg.packageName) != change.pkg) break;
                    listener.onModeChanged(change.pkg, change.op, change.oldMode, change.newMode);
                }
            } finally { Binder.restoreCallingIdentity(identity); }
            notifyMode(change.pkg, change.op, false);
        }
    }
    private void permissionModesChanged(int uid, Map<Integer, Integer> before, Map<Integer, Integer> after, Object publicationLock) {
        List<Change> changes = new ArrayList<>();
        for (ApkPackage pkg : packagesForUid(uid)) for (var entry : after.entrySet()) {
            Integer old = before.get(entry.getKey());
            if (old != null && !old.equals(entry.getValue())) {
                changes.add(new Change(pkg, entry.getKey(), old, entry.getValue()));
            }
        }
        // Permission publication can originate inside a package transaction. Enqueue without
        // acquiring the operation monitor; active-state transitions and callbacks run afterward.
        dispatch(() -> {
            if (publicationLock != null) { synchronized (publicationLock) { } }
            changed(changes);
        });
    }
    @Override public void resetAllModes(int user, String name) {
        enforce(Manifest.permission.MANAGE_APP_OPS_MODES);
        if (user != UserHandle.USER_ALL && user != 0) throw new SecurityException("Unknown user " + user);
        List<Change> changes;
        synchronized (mMutationLock) {
            synchronized (mPm) {
                State before = mStore.state();
                Map<UidOp, UidMode> uids = new HashMap<>(before.uids());
                Map<PackageOp, PackageMode> packages = new HashMap<>(before.packages());
                ApkPackage selected = name == null ? null : mPm.get(name);
                if (name != null && selected == null) return;
                Set<Integer> ops = new HashSet<>();
                uids.entrySet().removeIf(e -> {
                    boolean reset = (name == null || e.getKey().uid() == selected.uid())
                            && AppOpsManager.opAllowsReset(e.getKey().op());
                    if (reset) ops.add(e.getKey().op()); return reset;
                });
                packages.entrySet().removeIf(e -> {
                    boolean reset = (name == null || e.getKey().name().equals(name))
                            && AppOpsManager.opAllowsReset(e.getKey().op());
                    if (reset) ops.add(e.getKey().op()); return reset;
                });
                State after = new State(uids, packages);
                changes = changes(before, after, new ArrayList<>(mPm.packages()), ops);
                if (!before.equals(after)) mStore.commit(after);
            }
        }
        changed(changes);
    }

    /** Current foreground capabilities are supplied only by authenticated process lifecycle. */
    record UidState(int state, int capabilities) {
        static UidState absent() { return new UidState(UID_STATE_CACHED, 0); }
    }
    private record PendingState(UidState state, long deadline) { }
    void requestUidStateRefresh() {
        if (!mRefreshQueued.compareAndSet(false, true)) return;
        mEvents.execute(() -> { mRefreshQueued.set(false); refreshUidStates(); });
    }
    private void refreshUidStates() {
        synchronized (mUidRefreshLock) { refreshUidStatesLocked(); }
    }
    private void refreshUidStatesLocked() {
        Map<Integer, UidState> observed = mStateSource.get();
        Map<Integer, UidState> changed = new HashMap<>();
        long now = SystemClock.elapsedRealtime();
        synchronized (mLock) {
            Set<Integer> uids = new HashSet<>(mUidStates.keySet()); uids.addAll(observed.keySet());
            for (int uid : uids) {
                UidState old = mUidStates.getOrDefault(uid, UidState.absent());
                UidState next = observed.getOrDefault(uid, UidState.absent());
                PendingState pending = mPendingStates.get(uid);
                boolean capabilityLost = (old.capabilities & next.capabilities) != old.capabilities;
                boolean immediate = !observed.containsKey(uid) || next.state < old.state
                        || (next.state <= UID_STATE_MAX_LAST_NON_RESTRICTED && !capabilityLost);
                if (old.equals(next)) { mPendingStates.remove(uid); cancelStateTimer(uid); continue; }
                if (!immediate && pending == null) {
                    long delay = old.state <= UID_STATE_FOREGROUND_SERVICE ? 5000 : 1000;
                    pending = new PendingState(next, now + delay);
                    mPendingStates.put(uid, pending);
                    mStateTimers.put(uid, mEvents.schedule(this::refreshUidStates, delay, TimeUnit.MILLISECONDS));
                } else if (!immediate && pending != null && !next.equals(pending.state)) {
                    pending = new PendingState(next, pending.deadline); mPendingStates.put(uid, pending);
                }
                if (immediate || pending != null && now >= pending.deadline) {
                    if (observed.containsKey(uid)) mUidStates.put(uid, next); else mUidStates.remove(uid);
                    mPendingStates.remove(uid); cancelStateTimer(uid); changed.put(uid, old);
                    for (Client client : mClients.values()) for (Started started : client.starts.values()) {
                        if (started.key.uid == uid && started.running) moveRunningState(started);
                    }
                }
            }
        }
        if (!changed.isEmpty()) {
            refreshRunningOperations();
            for (int uid : changed.keySet()) for (ApkPackage pkg : packagesForUid(uid)) {
                for (int op = 0; op < _NUM_OP; op++) {
                    if (AppOpsManager.opToSwitch(op) == op && checkRawMode(op, uid, pkg.packageName) == MODE_FOREGROUND
                            && evaluateForeground(op, changed.get(uid)) != effective(op, uid, pkg.packageName)) {
                        notifyMode(pkg, op, true);
                    }
                }
            }
        }
    }
    private void cancelStateTimer(int uid) {
        java.util.concurrent.ScheduledFuture<?> timer = mStateTimers.remove(uid);
        if (timer != null) timer.cancel(false);
    }
    private int evaluate(int code, int uid, int mode) {
        if (mode != MODE_FOREGROUND) return mode;
        return evaluateForeground(AppOpsManager.opToSwitch(code), mUidStates.getOrDefault(uid, UidState.absent()));
    }
    private int evaluateForeground(int code, UidState state) {
        int capability = switch (code) {
            case OP_FINE_LOCATION, OP_COARSE_LOCATION, OP_MONITOR_LOCATION, OP_MONITOR_HIGH_POWER_LOCATION -> ActivityManager.PROCESS_CAPABILITY_FOREGROUND_LOCATION;
            case OP_CAMERA -> ActivityManager.PROCESS_CAPABILITY_FOREGROUND_CAMERA;
            case OP_RECORD_AUDIO, OP_RECEIVE_EXPLICIT_USER_INTERACTION_AUDIO -> ActivityManager.PROCESS_CAPABILITY_FOREGROUND_MICROPHONE;
            case OP_CONTROL_AUDIO -> ActivityManager.PROCESS_CAPABILITY_FOREGROUND_AUDIO_CONTROL;
            default -> 0;
        };
        boolean allowed = capability == 0 ? state.state <= AppOpsManager.resolveFirstUnrestrictedUidState(code)
                : (state.capabilities & capability) != 0;
        return allowed ? MODE_ALLOWED : MODE_IGNORED;
    }

    private record OpKey(int op, int uid, String name, String tag, int device) { }
    private static final class History {
        final LongSparseArray<AppOpsManager.NoteOpEvent> access = new LongSparseArray<>();
        final LongSparseArray<AppOpsManager.NoteOpEvent> reject = new LongSparseArray<>();
        int active;
    }
    private final class Started {
        final OpKey key;
        final ApkPackage owner;
        final boolean startDefault;
        final int attributionFlags;
        final int chain;
        final int operationFlags;
        final AppOpsManager.OpEventProxyInfo proxy;
        long stateKey;
        int nesting = 1;
        boolean running;
        boolean everStarted;
        long startTime;
        long wallTime;
        Started(OpKey key, boolean startDefault, int flags, int chain, int operationFlags, AppOpsManager.OpEventProxyInfo proxy) {
            this.key = key; this.owner = mPm.get(key.name); this.startDefault = startDefault; attributionFlags = flags; this.chain = chain;
            this.operationFlags = operationFlags; this.proxy = proxy;
        }
    }
    private final class Client implements IBinder.DeathRecipient {
        final IBinder token;
        final Map<OpKey, Started> starts = new HashMap<>();
        volatile boolean dead;
        Client(IBinder token) { this.token = token; }
        @Override public void binderDied() {
            dead = true;
            synchronized (mLock) {
                if (mClients.get(token) == this) mClients.remove(token);
                for (Started started : starts.values()) stopRunning(started);
                starts.clear();
            }
        }
    }
    private Client client(IBinder token) {
        Objects.requireNonNull(token, "AppOps client token");
        synchronized (mClientRegistration) {
            synchronized (mLock) {
                Client existing = mClients.get(token);
                if (existing != null) return existing.dead ? null : existing;
            }
            Client created = new Client(token);
            try { token.linkToDeath(created, 0); }
            catch (RemoteException dead) { return null; }
            synchronized (mLock) {
                if (!created.dead) { mClients.put(token, created); return created; }
            }
            token.unlinkToDeath(created, 0);
            return null;
        }
    }
    private void releaseEmpty(Client client) {
        synchronized (mClientRegistration) {
            boolean removed;
            synchronized (mLock) {
                removed = client.starts.isEmpty() && mClients.get(client.token) == client;
                if (removed) mClients.remove(client.token);
            }
            if (removed) client.token.unlinkToDeath(client, 0);
        }
    }
    private long eventKey(int uid, int flags) {
        return AppOpsManager.makeKey(mUidStates.getOrDefault(uid, UidState.absent()).state, flags);
    }
    private void record(OpKey key, boolean allowed, int flags, AppOpsManager.OpEventProxyInfo proxy) {
        History history = mHistory.computeIfAbsent(key, ignored -> new History());
        (allowed ? history.access : history.reject).put(eventKey(key.uid, flags),
                new AppOpsManager.NoteOpEvent(System.currentTimeMillis(), -1, proxy));
    }
    private void startRunning(Started started) {
        if (started.running) return;
        started.running = true; started.startTime = SystemClock.elapsedRealtime(); started.wallTime = System.currentTimeMillis();
        History history = mHistory.computeIfAbsent(started.key, ignored -> new History());
        if (history.active++ == 0) notifyActive(started, true);
        started.stateKey = eventKey(started.key.uid, started.operationFlags);
        history.access.put(started.stateKey, new AppOpsManager.NoteOpEvent(started.wallTime, -1, started.proxy));
        if (started.everStarted) notifyStarted(started.key, MODE_ALLOWED, started.operationFlags,
                started.attributionFlags, started.chain, START_TYPE_RESUMED);
        started.everStarted = true;
    }
    private void moveRunningState(Started started) {
        int mode = restricted(started.key.op, started.key.uid, started.key.name, started.key.tag, false)
                ? MODE_IGNORED : evaluate(started.key.op, started.key.uid, checkRawMode(started.key.op, started.key.uid, started.key.name));
        if (!shouldStart(mode, started.startDefault)) { stopRunning(started); return; }
        long next = eventKey(started.key.uid, started.operationFlags);
        if (next == started.stateKey) return;
        History history = mHistory.get(started.key);
        long now = SystemClock.elapsedRealtime();
        history.access.put(started.stateKey, new AppOpsManager.NoteOpEvent(started.wallTime,
                Math.max(0, now - started.startTime), started.proxy));
        started.startTime = now; started.wallTime = System.currentTimeMillis(); started.stateKey = next;
        history.access.put(next, new AppOpsManager.NoteOpEvent(started.wallTime, -1, started.proxy));
    }
    private void stopRunning(Started started) {
        if (!started.running) return;
        started.running = false;
        History history = mHistory.get(started.key);
        if (history == null) return;
        history.access.put(started.stateKey, new AppOpsManager.NoteOpEvent(started.wallTime,
                Math.max(0, SystemClock.elapsedRealtime() - started.startTime), started.proxy));
        if (--history.active == 0) notifyActive(started, false);
    }
    private static boolean shouldStart(int mode, boolean startDefault) {
        return mode == MODE_ALLOWED || mode == MODE_DEFAULT && startDefault;
    }
    private void refreshRunningOperations() {
        synchronized (mLock) {
            for (Client client : mClients.values()) for (Started started : client.starts.values()) {
                int mode = mPm.get(started.key.name) != started.owner || restricted(started.key.op, started.key.uid, started.key.name, started.key.tag, false)
                        ? MODE_IGNORED : evaluate(started.key.op, started.key.uid, checkRawMode(started.key.op, started.key.uid, started.key.name));
                if (shouldStart(mode, started.startDefault)) startRunning(started); else stopRunning(started);
            }
        }
    }
    private SyncNotedAppOp result(int mode, OpKey key) {
        return new SyncNotedAppOp(mode, key.op, key.tag, key.name);
    }
    private OpKey key(int code, int uid, String name, String tag, int device) {
        validateOp(code);
        return new OpKey(code, uid, name, attribution(packageFor(uid, name), tag), device);
    }
    @Override public SyncNotedAppOp noteOperation(int code, int uid, String name, String tag,
            boolean collect, String message, boolean collectMessage) {
        return noteOperationForDevice(code, uid, name, tag, 0, collect, message, collectMessage);
    }
    @Override public SyncNotedAppOp noteOperationForDevice(int code, int uid, String name, String tag, int device,
            boolean collect, String message, boolean collectMessage) {
        verifyIncomingUid(uid); verifyReadableOp(code);
        if (device != 0) return new SyncNotedAppOp(MODE_IGNORED, code, tag, name);
        OpKey key = key(code, uid, name, tag, device);
        SyncNotedAppOp result = note(key, OP_FLAG_SELF, null);
        if (collect && result.getOpMode() == MODE_ALLOWED) collectAsync(key, message);
        return result;
    }
    private SyncNotedAppOp note(OpKey key, int flags, AppOpsManager.OpEventProxyInfo proxy) {
        if (key.device != 0) return result(MODE_IGNORED, key);
        refreshUidStates();
        int mode = restricted(key.op, key.uid, key.name, key.tag, false)
                ? MODE_IGNORED : effective(key.op, key.uid, key.name);
        synchronized (mLock) {
            if (packageFor(key.uid, key.name) != null) record(key, mode == MODE_ALLOWED, flags, proxy);
            notifyNoted(key, flags, mode);
        }
        return result(mode, key);
    }
    @Override public SyncNotedAppOp startOperation(IBinder token, int code, int uid, String name, String tag,
            boolean startDefault, boolean collect, String message, boolean collectMessage, int flags, int chain) {
        return startOperationForDevice(token, code, uid, name, tag, 0, startDefault, collect, message, collectMessage, flags, chain);
    }
    @Override public SyncNotedAppOp startOperationForDevice(IBinder token, int code, int uid, String name, String tag,
            int device, boolean startDefault, boolean collect, String message, boolean collectMessage, int flags, int chain) {
        verifyIncomingUid(uid); verifyReadableOp(code);
        if (device != 0) return new SyncNotedAppOp(MODE_IGNORED, code, tag, name);
        OpKey key = key(code, uid, name, tag, device);
        int dependency = switch (code) {
            case OP_RECORD_AUDIO_HOTWORD, OP_RECEIVE_AMBIENT_TRIGGER_AUDIO, OP_RECORD_AUDIO_SANDBOXED -> OP_RECORD_AUDIO;
            case OP_CAMERA_SANDBOXED -> OP_CAMERA;
            default -> OP_NONE;
        };
        if (dependency != OP_NONE) {
            int mode = checkOperationForDevice(dependency, uid, name, tag, device);
            if (mode != MODE_ALLOWED) return result(mode, key);
        }
        SyncNotedAppOp result = start(token, key, startDefault, flags, chain);
        if (collect && result.getOpMode() == MODE_ALLOWED) collectAsync(key, message);
        return result;
    }
    private SyncNotedAppOp start(IBinder token, OpKey key, boolean startDefault, int flags, int chain) {
        return start(token, key, startDefault, flags, chain, OP_FLAG_SELF, null);
    }
    private SyncNotedAppOp start(IBinder token, OpKey key, boolean startDefault, int flags, int chain,
            int operationFlags, AppOpsManager.OpEventProxyInfo proxy) {
        if (key.device != 0) return result(MODE_IGNORED, key);
        Objects.requireNonNull(token, "AppOps client token");
        refreshUidStates();
        int mode = key.device != 0 || restricted(key.op, key.uid, key.name, key.tag, false)
                ? MODE_IGNORED : effective(key.op, key.uid, key.name);
        if (!shouldStart(mode, startDefault)) {
            synchronized (mLock) {
                if (packageFor(key.uid, key.name) != null) record(key, false, operationFlags, proxy);
                notifyStarted(key, mode, operationFlags, flags, chain, START_TYPE_FAILED);
            }
            return result(mode, key);
        }
        synchronized (mClientRegistration) {
        Client client = client(token);
        if (client == null) return result(MODE_ERRORED, key);
        synchronized (mLock) {
            mode = key.device != 0 || restricted(key.op, key.uid, key.name, key.tag, false)
                    ? MODE_IGNORED : evaluate(key.op, key.uid, checkRawMode(key.op, key.uid, key.name));
            if (client.dead || mClients.get(token) != client) mode = MODE_ERRORED;
            if (shouldStart(mode, startDefault)) {
                Started started = client.starts.get(key);
                if (started != null && mPm.get(key.name) != started.owner) {
                    stopRunning(started); client.starts.remove(key); started = null;
                }
                if (started == null) {
                    started = new Started(key, startDefault, flags, chain, operationFlags, proxy); client.starts.put(key, started);
                    startRunning(started);
                } else started.nesting++;
                notifyStarted(key, MODE_ALLOWED, operationFlags, flags, chain, START_TYPE_STARTED);
                return result(MODE_ALLOWED, key);
            }
        }
        releaseEmpty(client);
        return result(mode, key);
        }
    }
    @Override public void finishOperation(IBinder token, int code, int uid, String name, String tag) {
        finishOperationForDevice(token, code, uid, name, tag, 0);
    }
    @Override public void finishOperationForDevice(IBinder token, int code, int uid, String name, String tag, int device) {
        verifyIncomingUid(uid); verifyReadableOp(code);
        finish(token, key(code, uid, name, tag, device));
    }
    private void finish(IBinder token, OpKey key) {
        Client client;
        synchronized (mLock) {
            client = mClients.get(token);
            if (client == null) return;
            Started started = client.starts.get(key);
            if (started == null) return;
            if (--started.nesting == 0) { client.starts.remove(key); stopRunning(started); }
        }
        releaseEmpty(client);
    }
    @Override public boolean isOperationActive(int code, int uid, String name) {
        validateOp(code);
        if (!hasPermission(Manifest.permission.WATCH_APPOPS)) verifyIncomingUid(uid);
        refreshUidStates();
        synchronized (mLock) {
            for (var entry : mHistory.entrySet()) {
                OpKey key = entry.getKey();
                if (key.op == code && key.uid == uid && Objects.equals(key.name, name) && entry.getValue().active > 0) return true;
            }
        }
        return false;
    }

    private final class ModeWatcher implements IBinder.DeathRecipient {
        final IAppOpsCallback callback;
        final ApkPackage owner;
        final int uid;
        final Set<Watch> watches = new HashSet<>();
        volatile boolean dead;
        ModeWatcher(IAppOpsCallback callback, int uid) { this.callback = callback; this.uid = uid; this.owner = observerOwner(); }
        @Override public void binderDied() {
            dead = true;
            synchronized (mLock) { if (mWatchers.get(callback.asBinder()) == this) mWatchers.remove(callback.asBinder()); }
        }
    }
    private ApkPackage observerOwner() {
        return callers().isSystem() ? null : mPm.get(callers().callerPackage());
    }
    private boolean observerCurrent(ApkPackage owner) {
        return owner == null || packageFor(owner.uid(), owner.packageName) == owner;
    }
    private record Watch(int op, String name, int flags) { }
    @Override public void startWatchingMode(int code, String name, IAppOpsCallback callback) {
        startWatchingModeWithFlags(code, name, 0, callback);
    }
    @Override public void startWatchingModeWithFlags(int code, String name, int flags, IAppOpsCallback callback) {
        if (code != OP_NONE) verifyReadableOp(code);
        Objects.requireNonNull(callback);
        int uid = hasPermission(Manifest.permission.WATCH_APPOPS) ? -1 : callers().callerUid();
        if (uid != -1) callers().callerPackage();
        synchronized (mClientRegistration) {
            ModeWatcher watcher;
            synchronized (mLock) { watcher = mWatchers.get(callback.asBinder()); }
            if (watcher == null) {
                watcher = new ModeWatcher(callback, uid);
                try { callback.asBinder().linkToDeath(watcher, 0); } catch (RemoteException dead) { return; }
            }
            synchronized (mLock) {
                if (watcher.dead) return;
                watcher.watches.add(new Watch(code, name, flags)); mWatchers.put(callback.asBinder(), watcher);
            }
        }
    }
    @Override public void stopWatchingMode(IAppOpsCallback callback) {
        if (callback == null) return;
        ModeWatcher watcher;
        synchronized (mClientRegistration) {
            synchronized (mLock) { watcher = mWatchers.remove(callback.asBinder()); }
            if (watcher != null) { watcher.dead = true; callback.asBinder().unlinkToDeath(watcher, 0); }
        }
    }
    private void notifyMode(ApkPackage pkg, int op, boolean foreground) {
        List<Runnable> calls = new ArrayList<>();
        synchronized (mLock) {
            for (ModeWatcher watcher : mWatchers.values()) {
                if (watcher.uid != -1 && watcher.uid != pkg.uid()) continue;
                Set<Integer> notifications = new HashSet<>();
                for (Watch watch : watcher.watches) {
                    if (foreground && (watch.flags & WATCH_FOREGROUND_CHANGES) == 0) continue;
                    if (watch.op != OP_NONE && AppOpsManager.opToSwitch(watch.op) != op) continue;
                    if (watch.name != null && !watch.name.equals(pkg.packageName)) continue;
                    notifications.add(watch.op == OP_NONE || (watch.flags & CALL_BACK_ON_SWITCHED_OP) != 0 ? op : watch.op);
                }
                for (int notified : notifications) calls.add(() -> {
                    if (watcher.dead || !observerCurrent(watcher.owner) || packageFor(pkg.uid(), pkg.packageName) != pkg) return;
                    try { watcher.callback.opChanged(notified, pkg.uid(), pkg.packageName, "default:0"); }
                    catch (RemoteException dead) { watcher.binderDied(); }
                });
            }
        }
        for (Runnable call : calls) dispatch(call);
    }
    private void dispatch(Runnable action) {
        mEvents.execute(() -> {
            long identity = Binder.clearCallingIdentity();
            try { action.run(); } finally { Binder.restoreCallingIdentity(identity); }
        });
    }

    @Override public List<AppOpsManager.PackageOps> getPackagesForOps(int[] ops) {
        enforce(Manifest.permission.GET_APP_OPS_STATS); validateOps(ops);
        List<AppOpsManager.PackageOps> result = new ArrayList<>();
        for (ApkPackage pkg : mPm.packages()) {
            List<AppOpsManager.PackageOps> entries = packageOps(pkg.uid(), pkg.packageName, ops);
            if (entries != null) result.addAll(entries);
        }
        return result.isEmpty() ? null : result;
    }
    @Override public List<AppOpsManager.PackageOps> getOpsForPackage(int uid, String name, int[] ops) {
        enforce(Manifest.permission.GET_APP_OPS_STATS); validateOps(ops);
        return packageOps(uid, name, ops);
    }
    private static void validateOps(int[] ops) { if (ops != null) for (int op : ops) validateOp(op); }
    private static boolean includes(int[] ops, int op) { if (ops == null) return true; for (int value : ops) if (value == op) return true; return false; }
    private List<AppOpsManager.PackageOps> packageOps(int uid, String name, int[] requested) {
        if (packageFor(uid, name) == null) return null;
        refreshUidStates();
        Map<Integer, Map<String, AppOpsManager.AttributedOpEntry>> entries = new java.util.TreeMap<>();
        synchronized (mLock) {
            for (var entry : mHistory.entrySet()) {
                OpKey key = entry.getKey(); History history = entry.getValue();
                if (key.uid != uid || !key.name.equals(name) || !includes(requested, key.op)) continue;
                entries.computeIfAbsent(key.op, ignored -> new HashMap<>()).put(key.tag,
                        new AppOpsManager.AttributedOpEntry(key.op, history.active > 0, accessSnapshot(key, history), history.reject.clone()));
            }
        }
        for (PackageOp key : mStore.state().packages().keySet()) if (key.uid() == uid && key.name().equals(name)
                && includes(requested, key.op()) && mPm.permissions().runtimePermissionForOp(key.op()) == null) {
            entries.computeIfAbsent(key.op(), ignored -> new HashMap<>());
        }
        List<AppOpsManager.OpEntry> list = new ArrayList<>();
        for (var entry : entries.entrySet()) list.add(new AppOpsManager.OpEntry(entry.getKey(), checkRawMode(entry.getKey(), uid, name), entry.getValue()));
        return list.isEmpty() ? null : List.of(new AppOpsManager.PackageOps(name, uid, list));
    }
    private LongSparseArray<AppOpsManager.NoteOpEvent> accessSnapshot(OpKey key, History history) {
        LongSparseArray<AppOpsManager.NoteOpEvent> access = history.access.clone();
        if (history.active == 0) return access;
        long now = SystemClock.elapsedRealtime();
        for (Client client : mClients.values()) {
            Started started = client.starts.get(key);
            if (started == null || !started.running) continue;
            AppOpsManager.NoteOpEvent previous = access.get(started.stateKey);
            if (previous == null || previous.getNoteTime() <= started.wallTime) access.put(started.stateKey,
                    new AppOpsManager.NoteOpEvent(started.wallTime, Math.max(0, now - started.startTime), started.proxy));
        }
        return access;
    }
    @Override public List<AppOpsManager.PackageOps> getUidOps(int uid, int[] ops) {
        enforce(Manifest.permission.GET_APP_OPS_STATS); validateOps(ops);
        List<AppOpsManager.OpEntry> entries = new ArrayList<>();
        for (var entry : mStore.state().uids().entrySet()) {
            if (entry.getKey().uid() == uid && includes(ops, entry.getKey().op())
                    && mPm.permissions().runtimePermissionForOp(entry.getKey().op()) == null
                    && entry.getValue().owners().stream().anyMatch(this::validOwner)) {
                entries.add(new AppOpsManager.OpEntry(entry.getKey().op(), entry.getValue().mode(), Map.of()));
            }
        }
        if (!packagesForUid(uid).isEmpty()) for (int op = 0; op < _NUM_OP; op++) if (includes(ops, op)) {
            Integer mode = mPm.permissions().runtimeOpMode(op, uid);
            if (mode != null && mode != AppOpsManager.opToDefaultMode(op)) {
                entries.add(new AppOpsManager.OpEntry(op, mode, Map.of()));
            }
        }
        return entries.isEmpty() ? null : List.of(new AppOpsManager.PackageOps(null, uid, entries));
    }
    @Override public ParceledListSlice<AppOpsManager.PackageOps> getPackagesForOpsForDevice(int[] ops, String device) {
        enforce(Manifest.permission.GET_APP_OPS_STATS);
        if (!"default:0".equals(device)) return new ParceledListSlice<>(List.of());
        List<AppOpsManager.PackageOps> packages = getPackagesForOps(ops);
        return new ParceledListSlice<>(packages == null ? List.of() : packages);
    }
    @Override public List<String> getPackagesWithNonDefaultUidMode(int code, int mode, int user) {
        enforce(Manifest.permission.QUERY_ALL_PACKAGES); int op = switchOp(code); validateMode(mode);
        if (mode == AppOpsManager.opToDefaultMode(op)) throw new IllegalArgumentException("Requested mode is the operation default");
        if (user != 0) { enforce(Manifest.permission.INTERACT_ACROSS_USERS); return List.of(); }
        List<String> packages = new ArrayList<>();
        for (ApkPackage pkg : mPm.packages()) {
            UidMode value = mStore.state().uids().get(new UidOp(pkg.uid(), op));
            if (value != null && value.mode() == mode && value.owners().stream().anyMatch(this::validOwner)) packages.add(pkg.packageName);
        }
        return packages;
    }

    private void packageRemoved(String name) {
        List<Client> empty = new ArrayList<>();
        synchronized (mLock) {
            for (Client client : mClients.values()) {
                client.starts.entrySet().removeIf(entry -> {
                    if (!entry.getKey().name.equals(name)) return false;
                    stopRunning(entry.getValue()); return true;
                });
                if (client.starts.isEmpty()) empty.add(client);
            }
            mHistory.keySet().removeIf(key -> key.name.equals(name));
        }
        for (Client client : empty) releaseEmpty(client);
        retireObservers(name, null);
        pruneModes(name);
        // The installer cannot retire ownership durably while an old grant remains on disk.
        // This explicit lifecycle boundary runs outside the package and operation monitors.
        if (!mStore.flush()) throw new IllegalStateException("Cannot persist retired AppOps ownership");
    }
    private void packageChanged(ApkPackage pkg) {
        // Signed package generations own modes. A replaced APK cannot inherit an unrelated
        // identity's mode, and an obsolete process cannot keep attributed starts alive.
        List<Client> empty = new ArrayList<>();
        synchronized (mLock) {
            for (Client client : mClients.values()) {
                client.starts.entrySet().removeIf(entry -> {
                    Started started = entry.getValue();
                    if (!entry.getKey().name.equals(pkg.packageName) || started.owner == pkg) return false;
                    stopRunning(started); return true;
                });
                if (client.starts.isEmpty()) empty.add(client);
            }
        }
        for (Client client : empty) releaseEmpty(client);
        retireObservers(pkg.packageName, pkg);
        pruneModes(null);
        refreshRunningOperations();
        requestUidStateRefresh();
    }
    private void retireObservers(String name, ApkPackage current) {
        List<ModeWatcher> modes = new ArrayList<>();
        List<EventWatcher> events = new ArrayList<>();
        android.os.RemoteCallbackList<com.android.internal.app.IAppOpsAsyncNotedCallback> async = null;
        synchronized (mLock) {
            for (ModeWatcher watcher : mWatchers.values()) if (watcher.owner != null
                    && name.equals(watcher.owner.packageName) && watcher.owner != current) modes.add(watcher);
            for (EventWatcher watcher : mEventWatchers.values()) if (watcher.owner != null
                    && name.equals(watcher.owner.packageName) && watcher.owner != current) events.add(watcher);
            if (mAsyncOwners.get(name) != current || current == null) {
                async = mAsync.remove(name); mAsyncOwners.remove(name); mUnforwarded.remove(name);
            }
        }
        for (ModeWatcher watcher : modes) stopWatchingMode(watcher.callback);
        for (EventWatcher watcher : events) {
            watcher.binderDied(); watcher.binder.unlinkToDeath(watcher, 0);
        }
        if (async != null) async.kill();
    }
    private void pruneModes(String removed) {
        synchronized (mMutationLock) {
            synchronized (mPm) {
                State before = mStore.state();
                Map<UidOp, UidMode> uids = new HashMap<>();
                for (var entry : before.uids().entrySet()) {
                    List<Owner> owners = new ArrayList<>();
                    // A shared UID remains the same permission principal while any admitted
                    // member with its verified signing identity remains installed.
                    for (ApkPackage pkg : packagesForUid(entry.getKey().uid())) {
                        if (pkg.packageName.equals(removed)) continue;
                        String signer = AppOpsModeStore.signer(pkg);
                        if (entry.getValue().owners().stream().anyMatch(owner -> Objects.equals(owner.signer(), signer))) {
                            owners.add(mStore.owner(pkg));
                        }
                    }
                    if (!owners.isEmpty()) uids.put(entry.getKey(), new UidMode(entry.getValue().mode(), owners));
                }
                Map<PackageOp, PackageMode> packages = new HashMap<>(before.packages());
                packages.entrySet().removeIf(e -> e.getKey().name().equals(removed) || !validOwner(e.getValue().owner()));
                State after = new State(uids, packages);
                if (!before.equals(after)) mStore.commit(after);
            }
        }
    }

    /** Authenticate the chain's first source before blaming a proxy/receiver pair. */
    void verifyAttributionSource(AttributionSourceState source) {
        List<AttributionSourceState> sources = AppOpsAttribution.chain(source);
        CallerIdentity callers = callers();
        if (!callers.isSystem() && !mPm.permissions().isRegisteredAttributionSource(source)) {
            verifyIncomingUid(source.uid);
            if (source.pid != -1 && source.pid != callers.callerPid()
                    && !hasPermission(Manifest.permission.UPDATE_APP_OPS_STATS)) {
                throw new SecurityException("Attribution PID does not belong to caller");
            }
        }
        for (AttributionSourceState node : sources) {
            if (checkPackage(node.uid, node.packageName) != MODE_ALLOWED) throw new SecurityException("Unknown attribution owner");
        }
    }
    private boolean trustedSkip(AttributionSourceState source) {
        if (hasPermission(Manifest.permission.UPDATE_APP_OPS_STATS)) return true;
        List<AttributionSourceState> chain = AppOpsAttribution.chain(source);
        return source.uid != callers().callerUid() && mPm.permissions().isRegisteredAttributionSource(source)
                && (chain.size() == 1 || mPm.permissions().isRegisteredAttributionSource(chain.get(1)));
    }
    private boolean trustedProxy(AttributionSourceState source, AttributionSourceState receiver) {
        return callers().callerUid() == receiver.uid || mPm.permissions().checkUidPermission(source.uid,
                Manifest.permission.UPDATE_APP_OPS_STATS, source.deviceId) == PackageManager.PERMISSION_GRANTED;
    }
    private OpKey key(int op, AttributionSourceState source) {
        return key(op, source.uid, source.packageName, source.attributionTag, source.deviceId);
    }
    @Override public SyncNotedAppOp noteProxyOperation(int op, AttributionSource source, boolean collect,
            String message, boolean collectMessage, boolean skip) {
        return noteProxyOperationWithState(op, source.asState(), collect, message, collectMessage, skip);
    }
    @Override public SyncNotedAppOp noteProxyOperationWithState(int op, AttributionSourceState source, boolean collect,
            String message, boolean collectMessage, boolean skip) {
        verifyReadableOp(op); verifyAttributionSource(source);
        List<AttributionSourceState> chain = AppOpsAttribution.chain(source);
        if (chain.size() < 2) throw new IllegalArgumentException("Proxy operation requires a receiver");
        AttributionSourceState receiver = chain.get(1);
        if (source.deviceId != 0 || receiver.deviceId != 0) return result(MODE_IGNORED, key(op, receiver));
        boolean trusted = trustedProxy(source, receiver);
        OpKey proxied = key(op, receiver);
        if (!(skip && trustedSkip(source))) {
            SyncNotedAppOp proxy = note(key(op, source), trusted ? OP_FLAG_TRUSTED_PROXY : OP_FLAG_UNTRUSTED_PROXY, null);
            if (proxy.getOpMode() != MODE_ALLOWED) return result(proxy.getOpMode(), proxied);
        }
        SyncNotedAppOp result = note(proxied, trusted ? OP_FLAG_TRUSTED_PROXIED : OP_FLAG_UNTRUSTED_PROXIED,
                new AppOpsManager.OpEventProxyInfo(source.uid, source.packageName, attribution(packageFor(source.uid, source.packageName), source.attributionTag)));
        if (collect && result.getOpMode() == MODE_ALLOWED) collectAsync(proxied, message);
        return result;
    }
    @Override public SyncNotedAppOp startProxyOperation(IBinder token, int op, AttributionSource source,
            boolean startDefault, boolean collect, String message, boolean collectMessage, boolean skip,
            int proxyFlags, int receiverFlags, int chainId) {
        return startProxyOperationWithState(token, op, source.asState(), startDefault, collect, message,
                collectMessage, skip, proxyFlags, receiverFlags, chainId);
    }
    @Override public SyncNotedAppOp startProxyOperationWithState(IBinder token, int op, AttributionSourceState source,
            boolean startDefault, boolean collect, String message, boolean collectMessage, boolean skip,
            int proxyFlags, int receiverFlags, int chainId) {
        verifyReadableOp(op); verifyAttributionSource(source);
        List<AttributionSourceState> chain = AppOpsAttribution.chain(source);
        if (chain.size() < 2) throw new IllegalArgumentException("Proxy operation requires a receiver");
        AttributionSourceState receiver = chain.get(1);
        if (source.deviceId != 0 || receiver.deviceId != 0) return result(MODE_IGNORED, key(op, receiver));
        OpKey proxy = key(op, source), proxied = key(op, receiver);
        boolean omitProxy = skip && trustedSkip(source);
        refreshUidStates();
        int mode = proxied.device != 0 ? MODE_IGNORED : effective(op, proxied.uid, proxied.name);
        if (!shouldStart(mode, startDefault)) return result(mode, proxied);
        if (!omitProxy) {
            SyncNotedAppOp started = start(token, proxy, startDefault, proxyFlags, chainId,
                    trustedProxy(source, receiver) ? OP_FLAG_TRUSTED_PROXY : OP_FLAG_UNTRUSTED_PROXY, null);
            if (started.getOpMode() != MODE_ALLOWED) return result(started.getOpMode(), proxied);
        }
        SyncNotedAppOp result = start(token, proxied, startDefault, receiverFlags, chainId,
                trustedProxy(source, receiver) ? OP_FLAG_TRUSTED_PROXIED : OP_FLAG_UNTRUSTED_PROXIED,
                new AppOpsManager.OpEventProxyInfo(proxy.uid, proxy.name, proxy.tag));
        if (result.getOpMode() != MODE_ALLOWED && !omitProxy) finish(token, proxy);
        if (collect && result.getOpMode() == MODE_ALLOWED) collectAsync(proxied, message);
        return result;
    }
    @Override public void finishProxyOperation(IBinder token, int op, AttributionSource source, boolean skip) {
        finishProxyOperationWithState(token, op, source.asState(), skip);
    }
    @Override public void finishProxyOperationWithState(IBinder token, int op, AttributionSourceState source, boolean skip) {
        verifyReadableOp(op); verifyAttributionSource(source);
        List<AttributionSourceState> chain = AppOpsAttribution.chain(source);
        if (chain.size() < 2) throw new IllegalArgumentException("Proxy operation requires a receiver");
        if (!(skip && trustedSkip(source))) finish(token, key(op, source));
        finish(token, key(op, chain.get(1)));
    }
    @Override public boolean isProxying(int op, String proxyName, String proxyTag, int uid, String name) {
        validateOp(op); Objects.requireNonNull(proxyName); Objects.requireNonNull(name);
        List<AppOpsManager.PackageOps> packages = packageOps(uid, name, new int[]{op});
        if (packages == null || packages.isEmpty() || packages.get(0).getOps().isEmpty()) return false;
        AppOpsManager.OpEntry operation = packages.get(0).getOps().get(0);
        if (!operation.isRunning()) return false;
        AppOpsManager.OpEventProxyInfo proxy = operation.getLastProxyInfo(
                OP_FLAG_TRUSTED_PROXIED | OP_FLAG_UNTRUSTED_PROXIED);
        return proxy != null && callers().callerUid() == proxy.getUid()
                && proxyName.equals(proxy.getPackageName())
                && Objects.equals(proxyTag, proxy.getAttributionTag());
    }

    private final Map<IBinder, EventWatcher> mEventWatchers = new HashMap<>();
    private final class EventWatcher implements IBinder.DeathRecipient {
        final IBinder binder;
        final ApkPackage owner;
        final int uid;
        final Map<Integer, android.os.IInterface> callbacks = new HashMap<>();
        final Map<Integer, Set<Integer>> operations = new HashMap<>();
        volatile boolean dead;
        EventWatcher(IBinder binder, int uid) { this.binder = binder; this.uid = uid; this.owner = observerOwner(); }
        @Override public void binderDied() {
            dead = true;
            synchronized (mLock) { if (mEventWatchers.get(binder) == this) mEventWatchers.remove(binder); }
        }
    }
    private void watchEvents(int kind, int[] ops, android.os.IInterface callback) {
        Objects.requireNonNull(callback); Objects.requireNonNull(ops); validateOps(ops);
        for (int op : ops) verifyReadableOp(op);
        int uid = hasPermission(Manifest.permission.WATCH_APPOPS) ? -1 : callers().callerUid();
        if (uid != -1) callers().callerPackage();
        synchronized (mClientRegistration) {
            EventWatcher watcher;
            synchronized (mLock) { watcher = mEventWatchers.get(callback.asBinder()); }
            if (watcher == null) {
                watcher = new EventWatcher(callback.asBinder(), uid);
                try { watcher.binder.linkToDeath(watcher, 0); } catch (RemoteException dead) { return; }
            }
            synchronized (mLock) {
                if (watcher.dead) return;
                Set<Integer> operations = watcher.operations.computeIfAbsent(kind, ignored -> new HashSet<>());
                for (int op : ops) operations.add(op);
                watcher.callbacks.put(kind, callback); mEventWatchers.put(watcher.binder, watcher);
            }
        }
    }
    private void stopEvents(int kind, android.os.IInterface callback) {
        if (callback == null) return;
        synchronized (mClientRegistration) {
            EventWatcher watcher;
            boolean empty;
            synchronized (mLock) {
                watcher = mEventWatchers.get(callback.asBinder());
                if (watcher == null) return;
                watcher.callbacks.remove(kind); watcher.operations.remove(kind);
                empty = watcher.callbacks.isEmpty();
                if (empty) { watcher.dead = true; mEventWatchers.remove(watcher.binder); }
            }
            if (empty) watcher.binder.unlinkToDeath(watcher, 0);
        }
    }
    @Override public void startWatchingActive(int[] ops, com.android.internal.app.IAppOpsActiveCallback callback) { watchEvents(0, ops, callback); }
    @Override public void stopWatchingActive(com.android.internal.app.IAppOpsActiveCallback callback) { stopEvents(0, callback); }
    @Override public void startWatchingStarted(int[] ops, com.android.internal.app.IAppOpsStartedCallback callback) { watchEvents(1, ops, callback); }
    @Override public void stopWatchingStarted(com.android.internal.app.IAppOpsStartedCallback callback) { stopEvents(1, callback); }
    @Override public void startWatchingNoted(int[] ops, com.android.internal.app.IAppOpsNotedCallback callback) { watchEvents(2, ops, callback); }
    @Override public void stopWatchingNoted(com.android.internal.app.IAppOpsNotedCallback callback) { stopEvents(2, callback); }
    private interface EventCall { void call(android.os.IInterface callback) throws RemoteException; }
    private void notifyEvent(int kind, OpKey key, EventCall call) {
        for (EventWatcher watcher : mEventWatchers.values()) {
            if (watcher.uid != -1 && watcher.uid != key.uid) continue;
            Set<Integer> ops = watcher.operations.get(kind);
            if (ops == null || !ops.contains(key.op)) continue;
            android.os.IInterface callback = watcher.callbacks.get(kind);
            dispatch(() -> {
                synchronized (mLock) {
                    if (watcher.dead || !observerCurrent(watcher.owner) || watcher.callbacks.get(kind) != callback) return;
                }
                try { call.call(callback); } catch (RemoteException dead) { watcher.binderDied(); }
            });
        }
    }
    private void notifyActive(Started started, boolean active) {
        OpKey key = started.key;
        notifyEvent(0, key, callback -> ((com.android.internal.app.IAppOpsActiveCallback)callback).opActiveChanged(
                key.op, key.uid, key.name, key.tag, key.device, active, started.attributionFlags, started.chain));
    }
    private void notifyStarted(OpKey key, int mode, int operationFlags, int flags, int chain, int type) {
        notifyEvent(1, key, callback -> ((com.android.internal.app.IAppOpsStartedCallback)callback).opStarted(
                key.op, key.uid, key.name, key.tag, key.device, operationFlags, mode, type, flags, chain));
    }
    private void notifyNoted(OpKey key, int flags, int mode) {
        notifyEvent(2, key, callback -> ((com.android.internal.app.IAppOpsNotedCallback)callback).opNoted(
                key.op, key.uid, key.name, key.tag, key.device, flags, mode));
    }

    private final Map<String, android.os.RemoteCallbackList<com.android.internal.app.IAppOpsAsyncNotedCallback>> mAsync = new HashMap<>();
    private final Map<String, ApkPackage> mAsyncOwners = new HashMap<>();
    private final Map<String, java.util.ArrayDeque<android.app.AsyncNotedAppOp>> mUnforwarded = new HashMap<>();
    private void verifyOwnPackage(String name) {
        callers().requirePackage(name);
        if (mPm.get(name) == null || mPm.isRemovalPending(name)
                || packageFor(callers().callerUid(), name) == null && !callers().isSystem()) throw new SecurityException("Unknown package");
    }
    @Override public void startWatchingAsyncNoted(String name, com.android.internal.app.IAppOpsAsyncNotedCallback callback) {
        verifyOwnPackage(name); Objects.requireNonNull(callback);
        synchronized (mClientRegistration) {
            android.os.RemoteCallbackList<com.android.internal.app.IAppOpsAsyncNotedCallback> callbacks;
            android.os.RemoteCallbackList<com.android.internal.app.IAppOpsAsyncNotedCallback> stale = null;
            synchronized (mLock) {
                ApkPackage owner = mPm.get(name);
                if (mAsyncOwners.get(name) != owner) {
                    stale = mAsync.remove(name); mUnforwarded.remove(name);
                }
                callbacks = mAsync.computeIfAbsent(name, ignored -> new android.os.RemoteCallbackList<>());
                mAsyncOwners.put(name, owner);
            }
            if (stale != null) stale.kill();
            callbacks.register(callback);
        }
    }
    @Override public void stopWatchingAsyncNoted(String name, com.android.internal.app.IAppOpsAsyncNotedCallback callback) {
        verifyOwnPackage(name); Objects.requireNonNull(callback);
        android.os.RemoteCallbackList<com.android.internal.app.IAppOpsAsyncNotedCallback> callbacks;
        synchronized (mLock) { callbacks = mAsync.get(name); }
        if (callbacks != null) callbacks.unregister(callback);
    }
    @Override public List<android.app.AsyncNotedAppOp> extractAsyncOps(String name) {
        verifyOwnPackage(name);
        synchronized (mLock) {
            java.util.ArrayDeque<android.app.AsyncNotedAppOp> queue = mUnforwarded.remove(name);
            return queue == null ? null : new ArrayList<>(queue);
        }
    }
    private void collectAsync(OpKey key, String message) {
        Objects.requireNonNull(message);
        android.app.AsyncNotedAppOp event = new android.app.AsyncNotedAppOp(key.op, callers().callerUid(), key.tag, message, System.currentTimeMillis());
        dispatch(() -> {
            android.os.RemoteCallbackList<com.android.internal.app.IAppOpsAsyncNotedCallback> callbacks;
            synchronized (mLock) { callbacks = mAsync.get(key.name); }
            if (callbacks == null || packageFor(key.uid, key.name) == null) return;
            boolean sent = false;
            int size = callbacks.beginBroadcast();
            try {
                for (int i = 0; i < size; i++) try { callbacks.getBroadcastItem(i).opNoted(event); sent = true; }
                catch (RemoteException dead) { }
            } finally { callbacks.finishBroadcast(); }
            if (!sent) synchronized (mLock) {
                java.util.ArrayDeque<android.app.AsyncNotedAppOp> queue = mUnforwarded.computeIfAbsent(key.name, ignored -> new java.util.ArrayDeque<>());
                if (queue.size() == 10) queue.removeFirst();
                queue.addLast(event);
            }
        });
    }
    @Override public boolean shouldCollectNotes(int op) {
        validateOp(op);
        if (AppOpsManager.shouldForceCollectNoteForOp(op)) return true;
        String permission = AppOpsManager.opToPermission(op);
        android.content.pm.PermissionInfo info = permission == null ? null : mPm.permissions().getPermissionInfo(permission, null, 0);
        return info != null && info.isRuntime();
    }
    @Override public void noteOperationsInBatch(Map batched) {
        Objects.requireNonNull(batched);
        for (Object value : batched.entrySet()) {
            Map.Entry<?, ?> entry = (Map.Entry<?, ?>)value;
            if (!(entry.getKey() instanceof AppOpsManager.NotedOp op) || !(entry.getValue() instanceof Integer count) || count < 1) {
                throw new IllegalArgumentException("Invalid batched app op");
            }
            noteOperationForDevice(op.getOp(), op.getUid(), op.getPackageName(), op.getAttributionTag(),
                    op.getVirtualDeviceId(), op.getShouldCollectAsyncNotedOp(), op.getMessage(), op.getShouldCollectMessage());
        }
    }

    boolean canSkipProxy(AttributionSourceState source) { return trustedSkip(source); }
    void finishAttributed(IBinder token, int op, AttributionSourceState source) {
        finish(token, key(op, source));
    }

    private record AudioKey(int op, int usage) { }
    private record AudioRestriction(int mode, Set<String> exceptions) { }
    private final Map<AudioKey, AudioRestriction> mAudioRestrictions = new HashMap<>();
    private int mCameraAudioRestriction;
    private final Map<IBinder, UserRestrictions> mRestrictions = new HashMap<>();
    private final class UserRestrictions implements IBinder.DeathRecipient {
        final IBinder token;
        final Map<Integer, android.os.PackageTagsList> ops = new HashMap<>();
        volatile boolean dead;
        UserRestrictions(IBinder token) { this.token = token; }
        @Override public void binderDied() {
            dead = true;
            Set<Integer> changed;
            synchronized (mLock) {
                changed = new HashSet<>(ops.keySet()); ops.clear();
                if (mRestrictions.get(token) == this) mRestrictions.remove(token);
            }
            restrictionsChanged(changed);
        }
    }
    private boolean restricted(int op, int uid, String name, String tag, boolean checkOnly) {
        synchronized (mLock) {
            for (UserRestrictions restriction : mRestrictions.values()) {
                if (!restriction.ops.containsKey(op)) continue;
                var bypass = AppOpsManager.opAllowSystemBypassRestriction(op);
                if (uid == Process.SYSTEM_UID && bypass != null && bypass.isSystemUid) continue;
                android.os.PackageTagsList exceptions = restriction.ops.get(op);
                if (exceptions != null && (checkOnly ? exceptions.includes(name) : exceptions.contains(name, tag))) continue;
                return true;
            }
        }
        return false;
    }
    @Override public void setUserRestrictions(android.os.Bundle restrictions, IBinder token, int user) {
        if (!callers().isSystem()) throw new SecurityException("User restrictions require system authority");
        Objects.requireNonNull(restrictions); Objects.requireNonNull(token);
        for (int op = 0; op < _NUM_OP; op++) {
            String restriction = AppOpsManager.opToRestriction(op);
            if (restriction != null) setUserRestriction(op, restrictions.getBoolean(restriction, false), token, user, null);
        }
    }
    @Override public void setUserRestriction(int op, boolean restricted, IBinder token, int user,
            android.os.PackageTagsList exclusions) {
        enforce(Manifest.permission.MANAGE_APP_OPS_RESTRICTIONS); validateOp(op); Objects.requireNonNull(token);
        if (user != 0 && user != UserHandle.USER_ALL) throw new SecurityException("Unknown user " + user);
        boolean changed = false;
        synchronized (mClientRegistration) {
            UserRestrictions record;
            synchronized (mLock) { record = mRestrictions.get(token); }
            if (record == null && restricted) {
                record = new UserRestrictions(token);
                try { token.linkToDeath(record, 0); } catch (RemoteException dead) { return; }
            }
            if (record == null) return;
            boolean empty;
            synchronized (mLock) {
                if (record.dead) return;
                boolean previous = record.ops.containsKey(op);
                android.os.PackageTagsList old = record.ops.get(op);
                if (restricted) record.ops.put(op, exclusions); else record.ops.remove(op);
                changed = previous != restricted || restricted && !Objects.equals(old, exclusions);
                empty = record.ops.isEmpty();
                if (empty) mRestrictions.remove(token); else mRestrictions.put(token, record);
            }
            if (empty) token.unlinkToDeath(record, 0);
        }
        if (changed) restrictionsChanged(Set.of(op));
    }
    private void restrictionsChanged(Set<Integer> ops) {
        refreshRunningOperations();
        for (ApkPackage pkg : mPm.packages()) for (int op : ops) notifyMode(pkg, AppOpsManager.opToSwitch(op), false);
    }
    @Override public void setAudioRestriction(int code, int[] usages, int mode, String[] exceptions) {
        enforce(Manifest.permission.MANAGE_APP_OPS_MODES); validateOp(code); validateMode(mode); Objects.requireNonNull(usages);
        Set<String> packages = exceptions == null ? Set.of() : Set.of(exceptions);
        synchronized (mLock) {
            for (int usage : usages) {
                AudioKey key = new AudioKey(code, usage);
                if (mode == MODE_ALLOWED) mAudioRestrictions.remove(key);
                else mAudioRestrictions.put(key, new AudioRestriction(mode, packages));
            }
        }
        restrictionsChanged(Set.of(code));
    }
    @Override public void setCameraAudioRestriction(int mode) {
        enforce(Manifest.permission.MANAGE_APP_OPS_MODES);
        if (mode < android.hardware.camera2.CameraDevice.AUDIO_RESTRICTION_NONE
                || mode > android.hardware.camera2.CameraDevice.AUDIO_RESTRICTION_VIBRATION_SOUND) throw new IllegalArgumentException("Invalid camera audio restriction");
        synchronized (mLock) { mCameraAudioRestriction = mode; }
        restrictionsChanged(Set.of(OP_PLAY_AUDIO, OP_VIBRATE));
    }
    @Override public int checkAudioOperation(int code, int usage, int uid, String name) {
        verifyReadableOp(code);
        synchronized (mLock) {
            int behavior = android.media.AudioAttributes.SUPPRESSIBLE_USAGES.get(usage);
            boolean suppressible = behavior == android.media.AudioAttributes.SUPPRESSIBLE_NOTIFICATION
                    || behavior == android.media.AudioAttributes.SUPPRESSIBLE_CALL || behavior == android.media.AudioAttributes.SUPPRESSIBLE_ALARM;
            if (suppressible && mCameraAudioRestriction != android.hardware.camera2.CameraDevice.AUDIO_RESTRICTION_NONE
                    && (code == OP_VIBRATE || code == OP_PLAY_AUDIO
                            && mCameraAudioRestriction == android.hardware.camera2.CameraDevice.AUDIO_RESTRICTION_VIBRATION_SOUND)) return MODE_IGNORED;
            AudioRestriction restriction = mAudioRestrictions.get(new AudioKey(code, usage));
            if (restriction != null && !restriction.exceptions.contains(name)) return restriction.mode;
        }
        return checkOperation(code, uid, name);
    }
    @Override public void reloadNonHistoricalState() {
        enforce(Manifest.permission.MANAGE_APPOPS);
        // Android 16 AppOpService.readState is a no-op: published policy remains authoritative.
        // Keep the explicit flush boundary without replacing it with an older on-disk state.
        synchronized (mMutationLock) { mStore.flush(); }
    }

}
