/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.PermissionGroupInfo;
import android.content.pm.PermissionInfo;
import android.content.res.Resources;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.IdentityHashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;

/**
 * The host runtime-permission controller. The ATM authenticates the requesting
 * activity/process before entering here; intent extras never identify the owner.
 * The host's default-No consent dialog runs away from app/Binder/UI threads.
 * A background permission (ACCESS_BACKGROUND_LOCATION) is never granted by the
 * foreground Allow: like Android 16's PermissionController it has its own choice,
 * here a separate native dialog (see PermissionPolicy.planRequest).
 */
final class PermissionRequests {
    // Keep these values in sync with compat/credentials/permission_dialog.inc and
    // background_permission_dialog.inc. Cancellation is not a user denial, and a dialog
    // that could not be shown (UNAVAILABLE) is not even a dismissal.
    static final int UNAVAILABLE = -1, DISMISSED = 0, DENIED = 1, ALLOWED = 2, FOREGROUND_ONLY = 3, ALWAYS = 4;
    /**
     * Where a background choice is made. UPGRADE: the request dialog of a pre-R target
     * (PermissionController's upgrade dialog). SETTINGS: the app's own App info page.
     * REDIRECT: a target R+ background request, which Android sends to that Settings page
     * (GrantPermissionsViewModel.sendDirectlyToSettings).
     */
    enum BackgroundMode { UPGRADE, SETTINGS, REDIRECT }
    public interface Decision {
        /** The foreground dialog: DISMISSED, DENIED or ALLOWED. */
        int decide(String packageName, String permissionNames);
        /** The background dialog: UNAVAILABLE, DISMISSED, DENIED, FOREGROUND_ONLY or ALWAYS. */
        int decideBackground(String packageName, String permissionNames);
        /**
         * The legacy permission review dialog: UNAVAILABLE, DISMISSED or ALLOWED.
         * Editable groups start checked; managed permission rows retain their
         * enforced state. Only {@code changeable} rows may be updated in place
         * when the user confirms (ALLOWED).
         */
        default int decideReview(String packageName, String[] groupLabels, boolean[] enabled,
                boolean[] changeable) {
            return UNAVAILABLE;
        }
    }

    private interface Owner {
        ApkPackage code();
        default PermissionPolicy.ScopedOwner scoped() { return null; }
        default Runnable retire() { return null; }
        String name();
        boolean isCurrent();
        int check(String permission);
        void recordUserDecision(List<String> permissions, boolean allowed);
    }

    private final PackageManagerService mPm;
    private final Executor mExecutor;
    private final Executor mReviewExecutor;
    private final Decision mDecision;
    private final Set<String> mPending = new HashSet<>();
    private final Map<ApkPackage, CompletableFuture<Boolean>> mPendingReviews = new IdentityHashMap<>();

    PermissionRequests(PackageManagerService pm) {
        this(pm, Executors.newSingleThreadExecutor(r -> {
            Thread thread = new Thread(r, "permission-consent");
            thread.setDaemon(true);
            return thread;
        }), new ThreadPoolExecutor(1, 1, 0, TimeUnit.MILLISECONDS, new ArrayBlockingQueue<>(16), r -> {
            Thread thread = new Thread(r, "permission-review");
            thread.setDaemon(true);
            return thread;
        }), new Decision() {
            @Override public int decide(String packageName, String permissionNames) {
                return confirmDecision(packageName, permissionNames);
            }
            @Override public int decideBackground(String packageName, String permissionNames) {
                return confirmBackground(packageName, permissionNames);
            }
            @Override public int decideReview(String packageName, String[] groupLabels, boolean[] enabled,
                    boolean[] changeable) {
                return confirmReview(packageName, groupLabels, enabled, changeable);
            }
        });
    }

    /** Server-local injection for regression tests; never exposed through Binder or env. */
    PermissionRequests(PackageManagerService pm, Executor executor, Decision decision) {
        this(pm, executor, executor, decision);
    }

    private PermissionRequests(PackageManagerService pm, Executor executor, Executor reviewExecutor,
            Decision decision) {
        mPm = pm;
        mExecutor = executor;
        mReviewExecutor = reviewExecutor;
        mDecision = decision;
    }

    /** Other host consent flows need only the explicit-Allow result. */
    static boolean confirm(String packageName, String permissions) {
        return confirmDecision(packageName, permissions) == ALLOWED;
    }

    static int confirmDecision(String packageName, String permissions) {
        try {
            AccountSecretStore.ensureLoaded();
            return nativeConfirm(packageName, permissions);
        } catch (LinkageError | SecurityException e) {
            SystemServer.log("permission consent unavailable: " + e.getClass().getSimpleName());
            return DISMISSED;
        }
    }

    static int confirmBackground(String packageName, String permissions) {
        try {
            AccountSecretStore.ensureLoaded();
            int choice = nativeConfirmBackground(packageName, permissions);
            return choice == DISMISSED || choice == DENIED || choice == FOREGROUND_ONLY || choice == ALWAYS
                    ? choice : UNAVAILABLE;
        } catch (LinkageError | SecurityException e) {
            SystemServer.log("background permission consent unavailable: " + e.getClass().getSimpleName());
            return UNAVAILABLE;
        }
    }

    static int confirmReview(String packageName, String[] groupLabels, boolean[] enabled, boolean[] changeable) {
        try {
            AccountSecretStore.ensureLoaded();
            int choice = nativeReviewPermissions(packageName, groupLabels, enabled, changeable);
            return choice == DISMISSED || choice == ALLOWED ? choice : UNAVAILABLE;
        } catch (LinkageError | SecurityException e) {
            SystemServer.log("permission review unavailable: " + e.getClass().getSimpleName());
            return UNAVAILABLE;
        }
    }

    private static native int nativeConfirm(String packageLabel, String permissionNames);
    private static native int nativeConfirmBackground(String packageLabel, String permissionNames);
    private static native int nativeReviewPermissions(String packageLabel, String[] groupLabels,
            boolean[] enabled, boolean[] changeable);

    private boolean current(ApkPackage captured) {
        return !mPm.isRemovalPending(captured.packageName) && mPm.get(captured.packageName) == captured;
    }

    /**
     * The background dialog, then its commit. The policy rechecks this APK generation and the
     * granted foreground under the PM monitor, and stops the app only after releasing it.
     * Closing the dialog records nothing, except on the REDIRECT path, where Android counts
     * returning from Settings without a choice against the app.
     */
    private void chooseBackground(PermissionPolicy policy, ApkPackage captured, String background,
            boolean killOnRevoke, BackgroundMode mode) {
        boolean settings = mode != BackgroundMode.UPGRADE;
        if (!current(captured) || !policy.backgroundChoiceAvailable(captured, background, settings)) return;
        int choice = mDecision.decideBackground(captured.packageName, describe(policy, List.of(background)));
        if (choice == DENIED || choice == FOREGROUND_ONLY || choice == ALWAYS) {
            policy.recordBackgroundChoice(captured, background, choice, killOnRevoke, settings);
        } else if (choice == DISMISSED && mode == BackgroundMode.REDIRECT) {
            policy.recordBackgroundDismissal(captured, background);
        }
    }

    /**
     * An app opening its own App info (ACTION_APPLICATION_DETAILS_SETTINGS) while one of its
     * background permissions waits for the user's choice, as microG does after a request:
     * Android's Settings page offers that choice, so show the background dialog, then run
     * {@code completion} once the user has chosen. Returns false (nothing shown) when no
     * such choice is pending; the caller then opens the Control Center as before.
     */
    boolean backgroundSettings(ApkPackage captured, Runnable completion) {
        if (captured == null || completion == null) throw new IllegalArgumentException("Invalid settings request");
        PermissionPolicy policy = mPm.permissions();
        String background = policy.pendingBackgroundChoice(captured);
        if (background == null) return false;
        boolean accepted;
        synchronized (mPending) { accepted = mPending.add(captured.packageName); }
        if (!accepted) {
            // A consent dialog for this app is already open; Settings returns at once.
            completion.run();
            return true;
        }
        try {
            mExecutor.execute(() -> {
                try {
                    chooseBackground(policy, captured, background, true, BackgroundMode.SETTINGS);
                } catch (RuntimeException e) {
                    SystemServer.log("background permission choice failed for " + captured.packageName
                            + ": " + e.getClass().getSimpleName());
                } finally {
                    synchronized (mPending) { mPending.remove(captured.packageName); }
                }
                completion.run();
            });
        } catch (RuntimeException e) {
            synchronized (mPending) { mPending.remove(captured.packageName); }
            throw e;
        }
        return true;
    }

    /**
     * Run the pre-launch permission review for a legacy package off ATM/PM/Binder locks.
     * Concurrent launch attempts for the same installed generation share the in-flight future without
     * showing duplicate dialogs. Completes with {@code true} only when the user confirms
     * (or no review remains pending) and the package generation is still current.
     */
    CompletableFuture<Boolean> reviewPermissions(ApkPackage captured) {
        if (captured == null) throw new IllegalArgumentException("Invalid review request");
        return reviewPermissions(captured, null, () -> current(captured));
    }

    CompletableFuture<Boolean> reviewScopedPermissions(PermissionPolicy.ScopedOwner scoped,
            java.util.function.BooleanSupplier opening) {
        return reviewPermissions(scoped.code(), scoped, () -> scoped.canReviewBeforeLaunch(opening));
    }

    private CompletableFuture<Boolean> reviewPermissions(ApkPackage captured, PermissionPolicy.ScopedOwner scoped,
            java.util.function.BooleanSupplier currentOwner) {
        CompletableFuture<Boolean> future;
        synchronized (mPendingReviews) {
            CompletableFuture<Boolean> existing = mPendingReviews.get(captured);
            if (existing != null) return existing;
            future = new CompletableFuture<>();
            mPendingReviews.put(captured, future);
        }
        try {
            mReviewExecutor.execute(() -> {
                boolean proceed = false;
                try {
                    PermissionPolicy policy = mPm.permissions();
                    List<PermissionPolicy.ReviewGroup> groups = !currentOwner.getAsBoolean() ? List.of()
                            : scoped == null ? policy.pendingReviewGroups(captured)
                            : policy.pendingScopedReview(scoped, currentOwner);
                    if (!currentOwner.getAsBoolean()) {
                        proceed = false;
                    } else if (groups.isEmpty()) {
                        proceed = true;
                    } else {
                        String[] labels = describeGroups(policy, groups);
                        boolean[] enabled = new boolean[groups.size()];
                        boolean[] changeable = new boolean[groups.size()];
                        for (int i = 0; i < groups.size(); i++) {
                            enabled[i] = groups.get(i).enabled();
                            changeable[i] = groups.get(i).changeable();
                        }
                        int decision = mDecision.decideReview(captured.packageName, labels, enabled, changeable);
                        if (decision == ALLOWED && currentOwner.getAsBoolean()) {
                            Set<String> kept = new HashSet<>();
                            for (int i = 0; i < groups.size(); i++) {
                                if (groups.get(i).changeable() && enabled[i]) kept.add(groups.get(i).groupName());
                            }
                            proceed = (scoped == null ? policy.recordPermissionReview(captured, groups, kept)
                                    : policy.recordScopedPermissionReview(scoped, currentOwner, groups, kept))
                                    && currentOwner.getAsBoolean()
                                    && (scoped == null ? !policy.isReviewRequired(captured)
                                        : policy.pendingScopedReview(scoped, currentOwner).isEmpty());
                        }
                    }
                } catch (RuntimeException e) {
                    SystemServer.log("permission review failed for " + captured.packageName
                            + ": " + e.getClass().getSimpleName());
                } finally {
                    synchronized (mPendingReviews) {
                        mPendingReviews.remove(captured, future);
                    }
                    future.complete(proceed);
                }
            });
        } catch (RuntimeException e) {
            synchronized (mPendingReviews) {
                mPendingReviews.remove(captured, future);
            }
            future.complete(false);
            throw e;
        }
        return future;
    }

    private static String[] describeGroups(PermissionPolicy policy, List<PermissionPolicy.ReviewGroup> groups) {
        String[] labels = new String[groups.size()];
        for (int i = 0; i < groups.size(); i++) {
            PermissionPolicy.ReviewGroup group = groups.get(i);
            CharSequence label = null;
            PermissionGroupInfo groupInfo = policy.getPermissionGroupInfo(group.groupName(), 0);
            if (groupInfo != null && "android".equals(groupInfo.packageName) && groupInfo.labelRes != 0) {
                try { label = Resources.getSystem().getText(groupInfo.labelRes); }
                catch (RuntimeException ignored) { }
            }
            if (label == null && !group.permissions().isEmpty()) {
                PermissionInfo permInfo = policy.getPermissionInfo(group.permissions().get(0), "android", 0);
                if (permInfo != null && "android".equals(permInfo.packageName) && permInfo.labelRes != 0) {
                    try { label = Resources.getSystem().getText(permInfo.labelRes); }
                    catch (RuntimeException ignored) { }
                }
            }
            labels[i] = label != null
                    ? label + " \u202a(" + group.groupName() + ")\u202c"
                    : '\u202a' + group.groupName() + '\u202c';
            if (group.mixed()) labels[i] = com.gesher.app.locale.UiText.text(
                    "review_permission_adjustable", labels[i]);
        }
        return labels;
    }

    void request(ApkPackage captured, String[] requested, Consumer<Intent> completion) {
        if (captured == null) throw new IllegalArgumentException("Invalid runtime permission owner");
        PermissionPolicy policy = mPm.permissions();
        request(new Owner() {
            @Override public ApkPackage code() { return captured; }
            @Override public String name() { return captured.packageName; }
            @Override public boolean isCurrent() {
                return !mPm.isRemovalPending(name()) && mPm.get(name()) == captured;
            }
            @Override public int check(String permission) {
                return policy.checkPermission(name(), permission);
            }
            @Override public void recordUserDecision(List<String> permissions, boolean allowed) {
                policy.recordUserDecision(captured, permissions, allowed);
            }
        }, requested, completion);
    }

    void requestScoped(PermissionPolicy.ScopedOwner captured, String[] requested,
            Consumer<Intent> completion) {
        requestScoped(captured, requested, completion, null);
    }

    void requestScoped(PermissionPolicy.ScopedOwner captured, String[] requested,
            Consumer<Intent> completion, Runnable retire) {
        if (captured == null) throw new IllegalArgumentException("Invalid runtime permission owner");
        request(new Owner() {
            @Override public ApkPackage code() { return captured.code(); }
            @Override public PermissionPolicy.ScopedOwner scoped() { return captured; }
            @Override public Runnable retire() { return retire; }
            @Override public String name() { return captured.packageName(); }
            @Override public boolean isCurrent() { return captured.isCurrent(); }
            @Override public int check(String permission) { return captured.check(permission); }
            @Override public void recordUserDecision(List<String> permissions, boolean allowed) {
                captured.recordUserDecision(permissions, allowed);
            }
        }, requested, completion);
    }

    private void request(Owner owner, String[] requested, Consumer<Intent> completion) {
        if (requested == null || requested.length > 128) {
            throw new IllegalArgumentException("Invalid runtime permission request");
        }
        String[] names = requested.clone();
        for (String name : names) {
            if (name != null && name.length() > 300) {
                throw new IllegalArgumentException("Invalid permission name");
            }
        }
        boolean accepted;
        synchronized (mPending) { accepted = mPending.add(owner.name()); }
        if (!accepted) {
            int[] denied = new int[names.length];
            Arrays.fill(denied, PackageManager.PERMISSION_DENIED);
            completion.accept(result(names, denied));
            return;
        }
        try {
            mExecutor.execute(() -> {
                int[] grants = new int[names.length];
                Arrays.fill(grants, PackageManager.PERMISSION_DENIED);
                try {
                    if (mPm.isCohost()) {
                        if (owner.isCurrent()) {
                            int[] actual = Zygote.nativeCohostPermissionExchange(
                                    CohostPermissionHost.REQUEST, names);
                            if (actual != null && actual.length == names.length && owner.isCurrent()) {
                                grants = actual;
                            }
                        }
                    } else {
                        hostConsent(owner, names, grants);
                    }
                } catch (RuntimeException e) {
                    SystemServer.log("permission request denied for " + owner.name()
                            + ": " + e.getClass().getSimpleName());
                } finally {
                    synchronized (mPending) { mPending.remove(owner.name()); }
                }
                completion.accept(result(names, grants));
            });
        } catch (RuntimeException e) {
            synchronized (mPending) { mPending.remove(owner.name()); }
            throw e;
        }
    }

    private void hostConsent(Owner owner, String[] names, int[] grants) {
        PermissionPolicy policy = mPm.permissions();
        ApkPackage captured = owner.code();
        PermissionPolicy.ScopedOwner scoped = owner.scoped();
        PermissionPolicy.RequestPlan plan = !owner.isCurrent() ? PermissionPolicy.RequestPlan.NONE
                : scoped == null ? policy.planRequest(captured, names) : policy.planScopedRequest(scoped, names);
        List<String> prompt = plan.foreground();
        int decision = !prompt.isEmpty() && owner.isCurrent()
                ? mDecision.decide(owner.name(), describe(policy, prompt)) : DISMISSED;
        synchronized (mPm) {
            if (owner.isCurrent() && (decision == ALLOWED || decision == DENIED)) {
                owner.recordUserDecision(prompt, decision == ALLOWED);
            }
        }
        BackgroundMode mode = captured.parsed.getTargetSdkVersion() >= android.os.Build.VERSION_CODES.R
                ? BackgroundMode.REDIRECT : BackgroundMode.UPGRADE;
        for (String background : plan.background()) {
            boolean killOnRevoke = plan.foregroundAlreadyGranted().contains(background);
            if (scoped == null) {
                chooseBackground(policy, captured, background, killOnRevoke, mode);
            } else {
                chooseScopedBackground(policy, scoped, background, killOnRevoke, mode, owner.retire());
            }
        }
        synchronized (mPm) {
            if (!owner.isCurrent()) return;
            for (int i = 0; i < names.length; i++) {
                if (names[i] != null) grants[i] = owner.check(names[i]);
            }
        }
    }

    private void chooseScopedBackground(PermissionPolicy policy, PermissionPolicy.ScopedOwner owner,
            String background, boolean killOnRevoke, BackgroundMode mode, Runnable retire) {
        boolean settings = mode != BackgroundMode.UPGRADE;
        if (!policy.scopedBackgroundChoiceAvailable(owner, background, settings)) return;
        int choice = mDecision.decideBackground(owner.packageName(), describe(policy, List.of(background)));
        if (choice == DENIED || choice == FOREGROUND_ONLY || choice == ALWAYS) {
            policy.recordScopedBackgroundChoice(owner, background, choice, killOnRevoke, settings, retire);
        } else if (choice == DISMISSED && mode == BackgroundMode.REDIRECT) {
            policy.recordScopedBackgroundDismissal(owner, background);
        }
    }

    private static Intent result(String[] names, int[] grants) {
        return new Intent().putExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_NAMES, names)
                .putExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_RESULTS, grants)
                .putExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_DEVICE_ID, 0);
    }

    private static String describe(PermissionPolicy policy, List<String> names) {
        StringBuilder text = new StringBuilder();
        for (String name : names) {
            PermissionInfo info = policy.getPermissionInfo(name, "android", 0);
            CharSequence label = null;
            if (info != null && "android".equals(info.packageName) && info.labelRes != 0) {
                try { label = Resources.getSystem().getText(info.labelRes); }
                catch (Resources.NotFoundException ignored) { }
            }
            if (text.length() != 0) text.append("\r\n");
            // The native host dialog uses GDI: use LRE/PDF rather than FSI/PDI.
            // Keep the technical identifier and its parentheses LTR inside RTL labels.
            if (label != null) text.append(label).append(" \u202a(").append(name).append(")\u202c");
            else text.append('\u202a').append(name).append('\u202c');
        }
        return text.toString();
    }
}
