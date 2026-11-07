/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.Intent;
import android.content.IntentSender;
import android.content.pm.IPackageInstaller;
import android.content.pm.IPackageInstallerCallback;
import android.content.pm.IPackageInstallerSession;
import android.content.pm.PackageInstaller;
import android.content.pm.ParceledListSlice;
import android.content.pm.Signature;
import android.content.pm.parsing.ApkLite;
import android.content.pm.parsing.ApkLiteParseUtils;
import android.content.pm.parsing.result.ParseResult;
import android.content.pm.parsing.result.ParseTypeImpl;
import android.graphics.Bitmap;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;
import android.os.IBinder;
import android.os.ServiceSpecificException;
import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.FileVisitResult;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.*;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Supplier;

/** Full APK sessions with fresh host consent for every commit. No store receives silent-install authority. */
public final class PackageInstallerService extends IPackageInstaller.Default {
    static final long MAX_FILE_BYTES = 1024L * 1024 * 1024;
    static final long MAX_SESSION_BYTES = 2 * MAX_FILE_BYTES;
    static final long MAX_DEX_METADATA_BYTES = 64L * 1024 * 1024;
    private static final AtomicInteger TRACE_WRITES = new AtomicInteger();
    private static final AtomicInteger TRACE_REMOVALS = new AtomicInteger();
    private static final int MAX_SESSIONS = 16, MAX_OWNER_SESSIONS = 4, MAX_FILES = 64;
    interface Consent { boolean allow(String installer, String target, long version); }
    interface UninstallConsent { boolean allow(String requester, String target, long version); }

    private final PackageManagerService mPm;
    private final Executor mExecutor;
    private final Consent mConsent;
    private final UninstallConsent mUninstallConsent;
    private final Supplier<String> mTestCaller;
    private final InstallFileBridge.Transport mTransport;
    private final AtomicInteger mIds = new AtomicInteger(1);
    private final Map<Integer, Session> mSessions = new LinkedHashMap<>();
    private final Map<IBinder, CallbackOwner> mCallbacks = new HashMap<>();
    private final Map<String, String> mPendingRemovals = new HashMap<>();
    private final Executor mCallbackExecutor = Executors.newSingleThreadExecutor(r -> {
        Thread t = new Thread(r, "package-install-callbacks"); t.setDaemon(true); return t;
    });
    private volatile CallerIdentity mCallers;
    private volatile ActivityTaskManagerService mAtm;
    private volatile Installer mInstaller;

    PackageInstallerService(PackageManagerService pm) {
        this(pm, Executors.newSingleThreadExecutor(r -> {
            Thread t = new Thread(r, "package-install-consent"); t.setDaemon(true); return t;
        }), (installer, target, version) -> PermissionRequests.confirm(installer,
                com.gesher.app.locale.UiText.text("install_request", target, version)),
                null, InstallPipeTransport.TRANSPORT,
                (requester, target, version) -> PermissionRequests.confirm(requester,
                        com.gesher.app.locale.UiText.text("removal_request", target, version)));
    }
    /** Server-local test injection; never exposed through Binder, intent extras, or environment. */
    PackageInstallerService(PackageManagerService pm, Executor executor, Consent consent,
            Supplier<String> caller, InstallFileBridge.Transport transport) {
        this(pm, executor, consent, caller, transport, (requester, target, version) -> false);
    }
    PackageInstallerService(PackageManagerService pm, Executor executor, Consent consent,
            Supplier<String> caller, InstallFileBridge.Transport transport, UninstallConsent removal) {
        mPm = pm; mExecutor = executor; mConsent = consent; mTestCaller = caller; mTransport = transport;
        mUninstallConsent = removal;
        pm.onPackageRemoval(this::packageRemoved);
    }
    private void packageRemoved(String name) {
        List<Session> sessions;
        synchronized (mSessions) { sessions = new ArrayList<>(mSessions.values()); }
        for (Session session : sessions) if (name.equals(session.owner)) {
            synchronized (session) { session.destroy(); }
        }
        synchronized (mCallbacks) {
            for (CallbackOwner callback : new ArrayList<>(mCallbacks.values())) {
                if (name.equals(callback.pkg)) removeCallback(callback);
            }
        }
    }
    void attach(ActivityTaskManagerService atm, Installer installer) {
        if (mInstaller != null) throw new IllegalStateException("Package installer already attached");
        // Session metadata is not persisted. Only this service's private staging
        // directories survive a crashed server, never installed code or app data.
        File[] stale = new File(Device.dataDir(), "system/install-sessions").listFiles();
        if (stale != null) for (File f : stale) {
            if (f.getName().startsWith("session-") || f.getName().startsWith("commit-")) clean(f);
        }
        mAtm = atm; mCallers = new CallerIdentity(mPm, atm); mInstaller = installer;
    }
    private String caller() {
        if (mTestCaller != null) return mTestCaller.get();
        if (mCallers == null) throw new UnsupportedOperationException("No persistent package installer");
        return mCallers.callerPackage();
    }
    private String owner(String claimed, int userId) {
        String actual = caller();
        if (userId != 0) throw new SecurityException("Only user 0 is supported");
        if (claimed == null || (!"android".equals(actual) && !actual.equals(claimed))
                || (!"android".equals(claimed) && mPm.get(claimed) == null)) {
            throw new SecurityException("Unknown or foreign installer");
        }
        return claimed;
    }
    private void reapStaleSessions() {
        List<Session> snapshot;
        synchronized (mSessions) { snapshot = new ArrayList<>(mSessions.values()); }
        // Never acquire a session lock while holding the registry lock. A commit
        // already owns a private snapshot; destruction marks it ineligible for
        // consent/publication without deleting that worker's snapshot underneath it.
        for (Session s : snapshot) {
            synchronized (s) {
                if (mPm.get(s.owner) != s.ownerCode) s.destroy();
            }
        }
    }
    @Override public int createSession(PackageInstaller.SessionParams p, String pkg, String tag, int user) {
        ApkPackage ownerCode = mPm.get(pkg);
        String owner = owner(pkg, user);
        if (mInstaller == null) throw new UnsupportedOperationException("No persistent package installer");
        if (p == null || p.mode != PackageInstaller.SessionParams.MODE_FULL_INSTALL || p.isMultiPackage
                || p.isStaged || p.dataLoaderParams != null
                || p.volumeUuid != null
                || (p.installFlags & 0x00020000) != 0) { // AOSP INSTALL_APEX, hidden in framework export.
            throw new UnsupportedOperationException("Only ordinary full APK installs are supported");
        }
        if (p.sizeBytes < -1 || p.sizeBytes > MAX_SESSION_BYTES) {
            throw new IllegalArgumentException("Invalid installation size");
        }
        reapStaleSessions();
        if (ownerCode == null && !"android".equals(owner)) throw new SecurityException("Installer disappeared");
        // Client installFlags, permission grants, originating UID and USER_ACTION_NOT_REQUIRED
        // never select privileged behavior. Every caller, including Play, needs fresh consent.
        Session created;
        synchronized (mSessions) {
            if (mPm.get(owner) != ownerCode || mPm.isRemovalPending(owner)) {
                throw new SecurityException("Installer changed or is retiring");
            }
            int count = 0;
            for (Session s : mSessions.values()) if (owner.equals(s.owner)) count++;
            if (mSessions.size() >= MAX_SESSIONS || count >= MAX_OWNER_SESSIONS) {
                throw new IllegalStateException("Too many installation sessions");
            }
            try {
                File root = new File(Device.dataDir(), "system/install-sessions");
                Files.createDirectories(root.toPath());
                File stage = Files.createTempDirectory(root.toPath(), "session-").toFile();
                int id = mIds.getAndIncrement();
                created = new Session(id, owner, ownerCode, p.appPackageName, p.sizeBytes, stage);
                mSessions.put(id, created);
            } catch (IOException e) { throw io(); }
        }
        notify(created, c -> c.onSessionCreated(created.id));
        return created.id;
    }
    private Session owned(int id) {
        Session s; synchronized (mSessions) { s = mSessions.get(id); }
        if (s == null) throw new IllegalArgumentException("Unknown installation session");
        s.checkOwner(); return s;
    }
    @Override public IPackageInstallerSession openSession(int id) { return owned(id).binder; }
    @Override public void abandonSession(int id) { owned(id).abandon(); }
    @Override public void updateSessionAppIcon(int id, Bitmap icon) { owned(id); }
    @Override public void updateSessionAppLabel(int id, String label) { owned(id); }
    @Override public PackageInstaller.SessionInfo getSessionInfo(int id) {
        Session s; synchronized (mSessions) { s = mSessions.get(id); }
        if (s == null) return null; s.checkOwner(); return s.info();
    }
    @Override public ParceledListSlice<PackageInstaller.SessionInfo> getAllSessions(int user) {
        if (user != 0) throw new SecurityException("Only user 0 is supported");
        String owner = caller();
        List<PackageInstaller.SessionInfo> list = new ArrayList<>();
        List<Session> snapshot;
        synchronized (mSessions) { snapshot = new ArrayList<>(mSessions.values()); }
        for (Session s : snapshot) {
            if (s.ownerCurrent() && ("android".equals(owner) || owner.equals(s.owner))) list.add(s.info());
        }
        return new ParceledListSlice<>(list);
    }
    @Override public ParceledListSlice<PackageInstaller.SessionInfo> getMySessions(String pkg, int user) {
        String owner = owner(pkg, user);
        List<PackageInstaller.SessionInfo> list = new ArrayList<>();
        List<Session> snapshot;
        synchronized (mSessions) { snapshot = new ArrayList<>(mSessions.values()); }
        for (Session s : snapshot) {
            if (owner.equals(s.owner) && s.ownerCurrent()) list.add(s.info());
        }
        return new ParceledListSlice<>(list);
    }
    @Override public ParceledListSlice<PackageInstaller.SessionInfo> getStagedSessions() {
        caller(); return new ParceledListSlice<>(Collections.emptyList());
    }
    @Override public void registerCallback(IPackageInstallerCallback c, int user) {
        if (user != 0) throw new SecurityException("Only user 0 is supported");
        String provisional = mTestCaller != null ? mTestCaller.get()
                : mCallers == null ? null : mCallers.packageForPid(mCallers.callerPid());
        ApkPackage capturedCode = mPm.get(provisional);
        String pkg = caller();
        if (provisional == null ? !"android".equals(pkg) : !provisional.equals(pkg)) {
            throw new SecurityException("Callback owner changed during authentication");
        }
        if (c == null) throw new IllegalArgumentException("Missing callback");
        IBinder key = c.asBinder();
        if (key == null) throw new IllegalArgumentException("Missing callback Binder");
        synchronized (mCallbacks) {
            if (mPm.get(pkg) != capturedCode || mPm.isRemovalPending(pkg)) {
                throw new SecurityException("Callback owner changed or is retiring");
            }
            for (CallbackOwner old : new ArrayList<>(mCallbacks.values())) {
                if (mPm.get(old.pkg) != old.code) removeCallback(old);
            }
            CallbackOwner old = mCallbacks.get(key);
            if (old != null) {
                if (!old.pkg.equals(pkg)) throw new SecurityException("Foreign callback");
                return; // A new Java AIDL Proxy for the same Binder is the same registration.
            }
            if (mCallbacks.size() >= 64) throw new IllegalStateException("Too many callbacks");
            CallbackOwner entry = new CallbackOwner(pkg, capturedCode, c);
            try { key.linkToDeath(entry.death, 0); }
            catch (RemoteException e) { return; }
            mCallbacks.put(key, entry);
            if (!key.isBinderAlive()) removeCallback(entry);
        }
    }
    @Override public void unregisterCallback(IPackageInstallerCallback c) {
        String pkg = caller();
        synchronized (mCallbacks) {
            CallbackOwner owner = c == null ? null : mCallbacks.get(c.asBinder());
            if (owner != null && !"android".equals(pkg) && !pkg.equals(owner.pkg)) {
                throw new SecurityException("Foreign callback");
            }
            if (owner != null) removeCallback(owner);
        }
    }
    @Override public void setPermissionsResult(int id, boolean accepted) {
        throw new SecurityException("Only the host installation dialog can approve a session");
    }
    @Override public void installExistingPackage(String pkg, int flags, int reason, IntentSender receiver,
            int user, List<String> permissions) {
        caller();
        result(receiver, -1, pkg, PackageInstaller.STATUS_FAILURE_INCOMPATIBLE, "Additional users unsupported");
    }
    @Override public void uninstall(android.content.pm.VersionedPackage pkg, String claimed, int flags,
            IntentSender receiver, int user) {
        traceRemoval("installer", flags, user);
        int pid = mCallers == null ? -1 : mCallers.callerPid();
        ActivityTaskManagerService.ProcessRecord process = mAtm == null ? null : mAtm.callerProcess(pid);
        uninstallCaptured(pkg, claimed, flags, receiver, user, pid, process);
    }
    /** Legacy PackageManager deletion has the same host consent, version and identity policy. */
    void uninstallLegacy(String name, int version, android.content.pm.IPackageDeleteObserver observer,
            int user, int flags) {
        traceRemoval("legacy", flags, user);
        int pid = mCallers == null ? -1 : mCallers.callerPid();
        ActivityTaskManagerService.ProcessRecord process = mAtm == null ? null : mAtm.callerProcess(pid);
        // Capture the attached process before authentication, never a client-supplied owner.
        String claimed = process == null ? caller() : process.packageName;
        IntentSender receiver = new IntentSender((android.content.IIntentSender) new android.content.IIntentSender.Stub() {
            @Override public void send(int code, Intent result, String type, IBinder token,
                    android.content.IIntentReceiver finished, String permission, android.os.Bundle options)
                    throws RemoteException {
                if (observer != null) observer.packageDeleted(name,
                        result.getIntExtra(PackageInstaller.EXTRA_LEGACY_STATUS, -1));
            }
        });
        uninstallCaptured(new android.content.pm.VersionedPackage(name, version), claimed, flags,
                receiver, user, pid, process);
    }
    private static void traceRemoval(String route, int flags, int user) {
        if ("1".equals(System.getenv("GESHER_TRACE_INSTALL_SESSION"))
                && TRACE_REMOVALS.getAndIncrement() < 16) {
            SystemServer.log("package removal route=" + route + " flags=" + flags + " user=" + user);
        }
    }
    private void uninstallCaptured(android.content.pm.VersionedPackage pkg, String claimed, int flags,
            IntentSender receiver, int user, int pid, ActivityTaskManagerService.ProcessRecord capturedProcess) {
        ApkPackage ownerCode = mPm.get(claimed);
        String owner = owner(claimed, user);
        if (pkg == null || receiver == null || pkg.getPackageName() == null || pkg.getLongVersionCode() < -1) {
            throw new IllegalArgumentException("A package, version and result receiver are required");
        }
        String name = pkg.getPackageName();
        if (flags != 0 || mInstaller == null) {
            uninstallResult(receiver, name, PackageInstaller.STATUS_FAILURE_BLOCKED,
                    "Only complete ordinary APK removal is supported", -1); return;
        }
        ApkPackage target = mPm.get(name);
        if (!mInstaller.ordinaryInstalled(target)) {
            uninstallResult(receiver, name, PackageInstaller.STATUS_FAILURE_BLOCKED,
                    "System, addon or unknown package cannot be removed", -1); return;
        }
        if (pkg.getLongVersionCode() != -1 && pkg.getLongVersionCode() != target.versionCode()) {
            uninstallResult(receiver, name, PackageInstaller.STATUS_FAILURE_CONFLICT,
                    "Package version changed", -1); return;
        }
        final boolean system = mTestCaller != null || mCallers.isSystem();
        final ActivityTaskManagerService.ProcessRecord process = system ? null : capturedProcess;
        boolean pending;
        synchronized (mPendingRemovals) {
            int count = 0;
            for (String pendingOwner : mPendingRemovals.values()) if (owner.equals(pendingOwner)) count++;
            pending = mPendingRemovals.containsKey(name) || count >= MAX_OWNER_SESSIONS
                    || mPendingRemovals.size() >= MAX_SESSIONS;
            if (!pending) mPendingRemovals.put(name, owner);
        }
        if (pending) {
            uninstallResult(receiver, name, PackageInstaller.STATUS_FAILURE_BLOCKED,
                    "A removal is pending or the request limit was reached", -1); return;
        }
        try {
            mExecutor.execute(() -> remove(owner, ownerCode, target, receiver, pid, system, process));
        } catch (RuntimeException unavailable) {
            synchronized (mPendingRemovals) { mPendingRemovals.remove(name); }
            uninstallResult(receiver, name, PackageInstaller.STATUS_FAILURE,
                    "Removal worker unavailable", -1);
        }
    }

    private boolean removalCallerCurrent(String owner, ApkPackage code, int pid, boolean system,
            ActivityTaskManagerService.ProcessRecord process) {
        return mPm.get(owner) == code && (system || process != null
                && mAtm.callerProcess(pid) == process && !process.exiting
                && owner.equals(mCallers.packageForPid(pid)));
    }
    private void remove(String owner, ApkPackage ownerCode, ApkPackage target, IntentSender receiver,
            int pid, boolean system, ActivityTaskManagerService.ProcessRecord process) {
        String name = target.packageName;
        int status = PackageInstaller.STATUS_FAILURE_ABORTED, legacy = -5;
        String message = "Removal not approved";
        try {
            if (!removalCallerCurrent(owner, ownerCode, pid, system, process) || mPm.get(name) != target) {
                throw new SecurityException("Removal caller or target changed");
            }
            // Gesher extension: any authenticated app may ask the host for this one
            // removal. Neither a manifest claim nor a flag grants DELETE_PACKAGES,
            // and even a privileged caller cannot bypass this fresh default-No dialog.
            if (!mUninstallConsent.allow(owner, name, target.versionCode())) return;
            synchronized (mInstaller) {
                if (!removalCallerCurrent(owner, ownerCode, pid, system, process) || mPm.get(name) != target
                        || !mInstaller.ordinaryInstalled(target)) {
                    throw new SecurityException("Removal caller or target changed");
                }
                if (!mInstaller.uninstall(name)) throw new IllegalStateException("Package was not removed");
            }
            status = PackageInstaller.STATUS_SUCCESS; legacy = 1; message = "Removed";
        } catch (SecurityException changed) {
            status = PackageInstaller.STATUS_FAILURE_BLOCKED; legacy = -1;
            message = "Removal identity or target changed";
        } catch (IOException storage) {
            status = PackageInstaller.STATUS_FAILURE_STORAGE; legacy = -1;
            message = "Removal storage failure";
        } catch (RuntimeException failed) {
            status = PackageInstaller.STATUS_FAILURE; legacy = -1;
            message = "Removal failed: " + failed.getClass().getSimpleName();
        } finally {
            synchronized (mPendingRemovals) { mPendingRemovals.remove(name); }
            uninstallResult(receiver, name, status, message, legacy);
        }
    }
    private static void uninstallResult(IntentSender receiver, String name, int status, String message, int legacy) {
        Intent result = new Intent().putExtra(PackageInstaller.EXTRA_PACKAGE_NAME, name)
                .putExtra(PackageInstaller.EXTRA_STATUS, status)
                .putExtra(PackageInstaller.EXTRA_STATUS_MESSAGE, message)
                .putExtra(PackageInstaller.EXTRA_LEGACY_STATUS, legacy);
        try { receiver.getTarget().send(0, result, null, null, null, null, null); }
        catch (RemoteException | RuntimeException failed) {
            SystemServer.log("package removal result unavailable: " + failed.getClass().getSimpleName());
        }
    }
    private final class CallbackOwner {
        final String pkg;
        final ApkPackage code;
        final IPackageInstallerCallback callback;
        final IBinder binder;
        final IBinder.DeathRecipient death = () -> removeCallback(this);
        CallbackOwner(String pkg, ApkPackage code, IPackageInstallerCallback callback) {
            this.pkg = pkg; this.code = code; this.callback = callback; binder = callback.asBinder();
        }
    }
    private void removeCallback(CallbackOwner entry) {
        synchronized (mCallbacks) {
            if (mCallbacks.get(entry.binder) != entry) return;
            mCallbacks.remove(entry.binder);
            try { entry.binder.unlinkToDeath(entry.death, 0); }
            catch (RuntimeException ignored) { } // A dead Binder already discarded its recipients.
        }
    }
    private interface Callback { void send(IPackageInstallerCallback callback) throws RemoteException; }
    private void notify(Session s, Callback action) {
        Map<IBinder, CallbackOwner> list;
        synchronized (mCallbacks) { list = new HashMap<>(mCallbacks); }
        // Never invoke an app's Binder while holding a session or registry lock.
        mCallbackExecutor.execute(() -> {
            for (Map.Entry<IBinder, CallbackOwner> e : list.entrySet()) {
                CallbackOwner owner = e.getValue();
                if (mPm.get(owner.pkg) != owner.code) {
                    removeCallback(owner);
                    continue;
                }
                if (!s.owner.equals(owner.pkg) && !"android".equals(owner.pkg)) continue;
                try { action.send(owner.callback); }
                catch (RemoteException | RuntimeException ignored) {
                    removeCallback(owner);
                }
            }
        });
    }
    private static void result(IntentSender receiver, int id, String pkg, int status, String message) {
        if (receiver == null) return;
        Intent intent = new Intent().putExtra(PackageInstaller.EXTRA_SESSION_ID, id)
                .putExtra(PackageInstaller.EXTRA_STATUS, status)
                .putExtra(PackageInstaller.EXTRA_STATUS_MESSAGE, message);
        if (pkg != null) intent.putExtra(PackageInstaller.EXTRA_PACKAGE_NAME, pkg);
        try { receiver.getTarget().send(0, intent, null, null, null, null, null); }
        catch (RemoteException | RuntimeException e) {
            SystemServer.log("package install result unavailable: " + e.getClass().getSimpleName());
        }
    }
    private static ServiceSpecificException io() {
        return new ServiceSpecificException(android.system.OsConstants.EIO, "Installation I/O failure");
    }
    private static void clean(File directory) {
        // walkFileTree does not follow symbolic links, unlike File.isDirectory.
        // Every argument is a service-generated direct child of install-sessions.
        Path root = new File(Device.dataDir(), "system/install-sessions").toPath().toAbsolutePath().normalize();
        Path path = directory.toPath().toAbsolutePath().normalize();
        if (!root.equals(path.getParent())) throw new SecurityException("Invalid session cleanup path");
        try {
            Files.walkFileTree(path, new SimpleFileVisitor<Path>() {
                @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attrs) throws IOException {
                    Files.delete(file); return FileVisitResult.CONTINUE;
                }
                @Override public FileVisitResult postVisitDirectory(Path dir, IOException failure) throws IOException {
                    if (failure != null) throw failure;
                    Files.delete(dir); return FileVisitResult.CONTINUE;
                }
            });
        } catch (IOException e) {
            SystemServer.log("package session cleanup deferred: " + e.getClass().getSimpleName());
        }
    }

    public final class Session extends IPackageInstallerSession.Default {
        final int id;
        final String owner, expectedPackage;
        final ApkPackage ownerCode;
        final long expectedSize, created = System.currentTimeMillis();
        final File stage;
        final IPackageInstallerSession binder;
        final Map<String, InstallFileBridge> writes = new HashMap<>();
        final Map<String, InstallSessionMetadata.Digests> checksums = new HashMap<>();
        int tracedWrites;
        boolean sealed, committing, destroyed, writeFailed;
        float progress;
        long bytes;
        Session(int id, String owner, ApkPackage code, String pkg, long size, File stage) {
            this.id = id; this.owner = owner; ownerCode = code; expectedPackage = pkg;
            expectedSize = size; this.stage = stage;
            binder = StandIn.of(IPackageInstallerSession.class, this).asInterface();
        }
        void checkOwner() {
            String actual = caller();
            if (!"android".equals(actual) && !owner.equals(actual)) throw new SecurityException("Foreign session");
            if (mPm.get(owner) != ownerCode) throw new SecurityException("Installer code changed");
            if (destroyed) throw new IllegalStateException("Installation session closed");
        }
        void writable() { checkOwner(); if (sealed) throw new IllegalStateException("Session sealed"); }
        File file(String name) {
            InstallSessionMetadata.validName(name);
            String[] names = stage.list();
            if (names != null) for (String existing : names) {
                if (existing.equalsIgnoreCase(name) && !existing.equals(name)) {
                    throw new IllegalArgumentException("Installation filename aliases another file");
                }
            }
            return new File(stage, name);
        }
        private void traceWrite(String name) {
            if ("1".equals(System.getenv("GESHER_TRACE_INSTALL_SESSION")) && tracedWrites++ < 16
                    && TRACE_WRITES.getAndIncrement() < 256) {
                // The opt-in diagnostic never includes raw filenames, paths or app data.
                SystemServer.log("package-install-write category=" + InstallSessionMetadata.category(name));
            }
        }
        synchronized PackageInstaller.SessionInfo info() {
            PackageInstaller.SessionInfo i = new PackageInstaller.SessionInfo();
            i.sessionId = id; i.userId = 0; i.installerPackageName = owner;
            i.installerUid = ownerCode == null ? android.os.Process.SYSTEM_UID : ownerCode.uid();
            i.mode = PackageInstaller.SessionParams.MODE_FULL_INSTALL; i.progress = progress;
            i.sealed = sealed; i.active = !writes.isEmpty() || committing;
            i.sizeBytes = expectedSize; i.appPackageName = expectedPackage;
            i.requireUserAction = PackageInstaller.SessionParams.USER_ACTION_REQUIRED;
            i.createdMillis = created; i.updatedMillis = created; return i;
        }
        @Override public synchronized void setClientProgress(float value) {
            writable(); if (!Float.isFinite(value)) throw new IllegalArgumentException("Invalid progress");
            progress = Math.max(0, Math.min(1, value));
            PackageInstallerService.this.notify(this, c -> c.onSessionProgressChanged(id, progress));
        }
        @Override public synchronized void addClientProgress(float value) { setClientProgress(progress + value); }
        @Override public synchronized String[] getNames() {
            checkOwner(); String[] names = stage.list();
            if (names != null) Arrays.sort(names);
            return names == null ? new String[0] : names;
        }
        @Override public synchronized ParcelFileDescriptor openWrite(String name, long offset, long length) {
            writable(); traceWrite(name); File file = file(name);
            if (InstallSessionMetadata.auxiliary(name)) {
                throw new IllegalArgumentException("Use the dedicated session metadata API");
            }
            if (name.endsWith(".idsig")) {
                throw new UnsupportedOperationException("V4 filesystem verification sidecars unsupported");
            }
            return openWriter(file, offset, length, name.endsWith(".dm") ? MAX_DEX_METADATA_BYTES : MAX_FILE_BYTES);
        }
        private ParcelFileDescriptor openWriter(File file, long offset, long length, long limit) {
            String name = file.getName();
            if (offset < 0 || offset > file.length() || length < -1 || length > limit
                    || writes.containsKey(name) || (!file.exists() && getNames().length >= MAX_FILES)) {
                throw new IllegalArgumentException("Invalid write range or active writer");
            }
            if (PackageInstaller.ENABLE_REVOCABLE_FD) {
                throw new UnsupportedOperationException("Revocable installation descriptors unsupported");
            }
            try {
                InstallFileBridge bridge = new InstallFileBridge(file, offset, limit,
                        (growth) -> {
                            synchronized (Session.this) {
                                if (destroyed || growth > MAX_SESSION_BYTES - bytes) {
                                    throw new IOException("Session size or lifetime exceeded");
                                }
                                bytes += growth;
                            }
                        }, (complete) -> {
                            boolean retryCleanup;
                            synchronized (Session.this) {
                                writes.remove(name);
                                if (!complete && !destroyed) writeFailed = true;
                                retryCleanup = destroyed;
                            }
                            if (retryCleanup) clean(stage);
                        }, mTransport);
                writes.put(name, bridge); bridge.start(); return bridge.client();
            } catch (IOException e) { throw io(); }
        }
        @Override public synchronized ParcelFileDescriptor openRead(String name) {
            checkOwner(); if (writes.containsKey(name)) throw new IllegalStateException("APK write active");
            try { return ParcelFileDescriptor.open(file(name), ParcelFileDescriptor.MODE_READ_ONLY); }
            catch (IOException e) { throw io(); }
        }
        @Override public void write(String name, long offset, long length, ParcelFileDescriptor input) {
            throw new UnsupportedOperationException("Use bounded openWrite stream");
        }
        @Override public synchronized void close() { checkOwner(); }
        @Override public synchronized void seal() {
            writable();
            if (!writes.isEmpty() || writeFailed) throw new IllegalStateException("APK writes incomplete");
            sealed = true;
        }
        @Override public synchronized void commit(IntentSender receiver, boolean transferred) {
            checkOwner();
            if (receiver == null) throw new IllegalArgumentException("Missing status receiver");
            if (transferred || committing || !writes.isEmpty() || writeFailed) {
                throw new IllegalStateException("Session cannot be committed");
            }
            sealed = true; committing = true;
            try { mExecutor.execute(() -> install(receiver)); }
            catch (RuntimeException e) { committing = false; throw e; }
        }
        private void install(IntentSender receiver) {
            int status = PackageInstaller.STATUS_FAILURE_INVALID;
            String pkg = expectedPackage, message = "Invalid APK set";
            File frozen = null;
            try {
                frozen = Files.createTempDirectory(stage.toPath().getParent(), "commit-").toFile();
                File[] all = stage.listFiles();
                Map<String, File> sidecars = new HashMap<>();
                List<File> files = new ArrayList<>();
                if (all != null) for (File source : all) {
                    File snapshot = new File(frozen, source.getName());
                    Files.copy(source.toPath(), snapshot.toPath());
                    if (source.getName().equals(InstallSessionMetadata.APP_METADATA)) {
                        InstallSessionMetadata.validateMetadata(snapshot);
                    } else if (source.getName().endsWith(".dm")) {
                        sidecars.put(source.getName(), snapshot);
                    } else if (source.getName().endsWith(".idsig")
                            || InstallSessionMetadata.auxiliary(source.getName())) {
                        throw new IllegalArgumentException("Unsupported staged sidecar");
                    } else files.add(snapshot); // APK names are arbitrary; parse the actual bytes.
                }
                if (files.isEmpty()) throw new IllegalArgumentException("Empty APK set");
                ApkLite first = null; Signature[] signers = null; boolean base = false;
                Map<File, ApkLite> parsedFiles = new LinkedHashMap<>();
                for (File snapshot : files) {
                    ParseResult<ApkLite> parsed = ApkLiteParseUtils.parseApkLite(
                            ParseTypeImpl.forParsingWithoutPlatformCompat(), snapshot, 0);
                    if (parsed.isError()) throw new IllegalArgumentException("Invalid manifest");
                    ApkLite lite = parsed.getResult();
                    parsedFiles.put(snapshot, lite);
                    Signature[] current = ApkPackage.verifySignatures(snapshot.getPath());
                    if (current == null) throw new SecurityException("Store installs must be signed");
                    if (first == null) { first = lite; signers = current; }
                    else if (!first.getPackageName().equals(lite.getPackageName())
                            || first.getLongVersionCode() != lite.getLongVersionCode()
                            || !Arrays.equals(signers, current)) throw new SecurityException("Mismatched APK set");
                    if (lite.getSplitName() == null) { if (base) throw new IllegalArgumentException("Multiple bases"); base = true; }
                }
                if (!base) throw new IllegalArgumentException("Missing base APK");
                // A unique private canonical path bypasses the mutable-source cache. The
                // client has no writable FD to either snapshot or final installer input.
                File canonical = new File(frozen, "apks"); Files.createDirectory(canonical.toPath());
                Set<String> consumedChecksums = new HashSet<>();
                for (Map.Entry<File, ApkLite> entry : parsedFiles.entrySet()) {
                    File source = entry.getKey(); ApkLite lite = entry.getValue();
                    String split = lite.getSplitName();
                    if (split != null && !split.matches("[A-Za-z0-9_][A-Za-z0-9_.-]{0,120}")) {
                        throw new IllegalArgumentException("Invalid split name");
                    }
                    String name = split == null ? "base.apk" : "split_" + split + ".apk";
                    File dm = sidecars.remove(InstallSessionMetadata.dexName(source.getName()));
                    if (dm != null) {
                        ParseResult<?> valid = android.content.pm.dex.DexMetadataHelper.validateDexMetadataFile(
                                ParseTypeImpl.forParsingWithoutPlatformCompat(), dm.getPath(), lite.getPackageName(),
                                lite.getLongVersionCode());
                        if (valid.isError()) throw new IllegalArgumentException("Invalid dex metadata");
                        Files.move(dm.toPath(), new File(canonical, InstallSessionMetadata.dexName(name)).toPath());
                    }
                    InstallSessionMetadata.Digests digests = checksums.get(source.getName());
                    if (digests != null) {
                        digests.store(canonical, name); consumedChecksums.add(source.getName());
                    }
                    Files.move(source.toPath(), new File(canonical, name).toPath());
                }
                if (!sidecars.isEmpty()) throw new IllegalArgumentException("Dex metadata has no APK");
                if (consumedChecksums.size() != checksums.size()) throw new IllegalArgumentException("Checksums have no APK");
                File appMetadata = new File(frozen, InstallSessionMetadata.APP_METADATA);
                if (appMetadata.isFile()) Files.move(appMetadata.toPath(),
                        new File(canonical, InstallSessionMetadata.APP_METADATA).toPath());
                pkg = first.getPackageName();
                if (expectedPackage != null && !expectedPackage.equals(pkg)) throw new SecurityException("Wrong package");
                ApkPackage targetCode = mPm.get(pkg);
                if (!ownerCurrent()) throw new SecurityException("Installer changed");
                status = PackageInstaller.STATUS_FAILURE_ABORTED; message = "Installation not approved";
                if (!mConsent.allow(owner, pkg, first.getLongVersionCode())) return;
                // The installer's own lock covers both stale-consent checks and publication.
                synchronized (mInstaller) {
                    if (!ownerCurrent() || mPm.get(pkg) != targetCode) throw new SecurityException("Package code changed");
                    String[] installed = mInstaller.install(new File(canonical, "base.apk").getPath(),
                            new InstallSessionMetadata(canonical));
                    if (!pkg.equals(installed[0]) || mPm.get(pkg) == null) throw new IllegalStateException("Not published");
                }
                status = PackageInstaller.STATUS_SUCCESS; message = "Installed";
            } catch (SecurityException e) {
                status = PackageInstaller.STATUS_FAILURE_BLOCKED; message = "Package identity or consent changed";
            } catch (IOException e) {
                status = PackageInstaller.STATUS_FAILURE_STORAGE; message = "Installation storage failure";
            } catch (Exception e) {
                status = PackageInstaller.STATUS_FAILURE_INVALID; message = "Installation failed: " + e.getClass().getSimpleName();
            } finally {
                boolean success = status == PackageInstaller.STATUS_SUCCESS;
                result(receiver, id, pkg, status, message);
                PackageInstallerService.this.notify(this, c -> c.onSessionFinished(id, success));
                if (frozen != null) clean(frozen);
                destroy();
            }
        }
        synchronized boolean ownerCurrent() { return !destroyed && mPm.get(owner) == ownerCode; }
        @Override public synchronized void abandon() {
            checkOwner(); if (committing) throw new IllegalStateException("Commit active");
            destroy(); PackageInstallerService.this.notify(this, c -> c.onSessionFinished(id, false));
        }
        synchronized void destroy() {
            destroyed = true;
            for (InstallFileBridge bridge : new ArrayList<>(writes.values())) bridge.forceClose();
            synchronized (mSessions) { mSessions.remove(id); }
            clean(stage);
        }
        @Override public synchronized boolean isMultiPackage() { checkOwner(); return false; }
        @Override public synchronized boolean isStaged() { checkOwner(); return false; }
        @Override public synchronized int getInstallFlags() { checkOwner(); return 0; }
        @Override public synchronized int[] getChildSessionIds() { checkOwner(); return new int[0]; }
        @Override public synchronized int getParentSessionId() { checkOwner(); return -1; }
        @Override public void transfer(String pkg) { throw new UnsupportedOperationException("Transfer unsupported"); }
        @Override public void stageViaHardLink(String path) { throw new SecurityException("Host paths cannot be staged"); }
        @Override public void removeSplit(String split) { throw new UnsupportedOperationException("Inheritance unsupported"); }
        @Override public void addChildSessionId(int child) { throw new UnsupportedOperationException("Multi-package unsupported"); }
        @Override public void removeChildSessionId(int child) { throw new UnsupportedOperationException("Multi-package unsupported"); }
        @Override public void addFile(int location, String name, long length, byte[] metadata, byte[] signature) {
            throw new UnsupportedOperationException("Data loaders unsupported");
        }
        @Override public void removeFile(int location, String name) {
            throw new UnsupportedOperationException("Data loaders unsupported");
        }
        @Override public void requestUserPreapproval(PackageInstaller.PreapprovalDetails details, IntentSender receiver) {
            checkOwner();
            result(receiver, id, expectedPackage, PackageInstaller.STATUS_FAILURE_BLOCKED,
                    "Preapproval unsupported; commit requires the host installation dialog");
        }
        @Override public synchronized void setChecksums(String name, android.content.pm.Checksum[] supplied, byte[] signature) {
            writable(); file(name);
            if (supplied == null) throw new IllegalArgumentException("Missing checksums");
            if (supplied.length == 0) return;
            if (checksums.containsKey(name)) throw new IllegalStateException("Checksums already supplied");
            if (checksums.size() >= MAX_FILES) throw new IllegalStateException("Too many checksum records");
            if (name.endsWith(".dm") || name.endsWith(".idsig") || InstallSessionMetadata.auxiliary(name)) {
                throw new IllegalArgumentException("Checksums require an APK filename");
            }
            checksums.put(name, new InstallSessionMetadata.Digests(supplied, signature));
        }
        @Override public synchronized ParcelFileDescriptor openWriteAppMetadata() {
            writable();
            File file = file(InstallSessionMetadata.APP_METADATA);
            if (writes.containsKey(file.getName())) throw new IllegalStateException("App metadata write active");
            long previous = file.length();
            try { Files.deleteIfExists(file.toPath()); bytes -= previous; }
            catch (IOException e) { throw io(); }
            return openWriter(file, 0, -1, InstallSessionMetadata.MAX_METADATA);
        }
        @Override public synchronized ParcelFileDescriptor getAppMetadataFd() {
            checkOwner();
            if (committing) throw new IllegalStateException("Session committing");
            File file = file(InstallSessionMetadata.APP_METADATA);
            if (writes.containsKey(file.getName())) throw new IllegalStateException("App metadata write active");
            if (!file.isFile()) return null;
            try { return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY); }
            catch (IOException e) { throw io(); }
        }
        @Override public synchronized void removeAppMetadata() {
            writable();
            File file = file(InstallSessionMetadata.APP_METADATA);
            if (writes.containsKey(file.getName())) throw new IllegalStateException("App metadata write active");
            long previous = file.length();
            try { Files.deleteIfExists(file.toPath()); bytes -= previous; }
            catch (IOException e) { throw io(); }
        }
        @Override public List<String> fetchPackageNames() {
            checkOwner();
            if (params != null && params.appPackageName != null) {
                return Collections.singletonList(params.appPackageName);
            }
            return Collections.emptyList();
        }
    }
}
