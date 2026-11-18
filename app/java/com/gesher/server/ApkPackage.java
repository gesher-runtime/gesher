/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.ComponentName;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ComponentInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.content.pm.Signature;
import android.content.pm.SigningDetails;
import android.content.pm.parsing.ApkLite;
import android.content.pm.parsing.ApkLiteParseUtils;
import android.content.pm.parsing.result.ParseResult;
import android.content.pm.parsing.result.ParseTypeImpl;
import android.os.Process;
import android.util.apk.ApkSignatureSchemeV2Verifier;
import android.util.apk.ApkSignatureSchemeV3Verifier;
import android.util.apk.SignatureNotFoundException;

import com.android.internal.pm.parsing.AppInfoUtils;
import com.android.internal.pm.parsing.PackageInfoCommonUtils;
import com.android.internal.pm.parsing.PackageParser2;
import com.android.internal.pm.parsing.pkg.ParsedPackage;
import com.android.internal.pm.pkg.component.ParsedActivity;
import com.android.internal.pm.pkg.component.ParsedComponent;
import com.android.internal.pm.pkg.component.ParsedIntentInfo;
import com.android.internal.pm.pkg.component.ParsedMainComponent;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.security.cert.CertificateEncodingException;
import java.security.cert.X509Certificate;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * An installed APK, parsed the way PackageManagerService parses one at install: with
 * framework.jar's {@link PackageParser2} (ParsingPackageUtils: the whole manifest, with
 * services, receivers, providers, permissions, meta-data and intent filters). Its
 * {@link PackageInfo}, {@link ApplicationInfo} and {@link ActivityInfo}s come from
 * {@link PackageInfoCommonUtils}, the framework's PackageInfoUtils for packages outside
 * PackageManagerService, plus what installing adds (paths, uid, data directories,
 * enabled). Its signing certificates are collected, not verified (gesher does not verify
 * signatures): apps read their own (GET_SIGNATURES, GET_SIGNING_CERTIFICATES), as WhatsApp
 * does for its registration token.
 */
public final class ApkPackage {
    /** Publisher identity used for persistent app capabilities across safe updates. */
    public String codeIdentity() { return NotificationManagerService.codeIdentity(this); }
    /** Flags for a PackageInfo with everything: every component, meta-data, permissions. */
    public static final long ALL = PackageManager.GET_ACTIVITIES
            | PackageManager.GET_RECEIVERS | PackageManager.GET_SERVICES
            | PackageManager.GET_PROVIDERS | PackageManager.GET_META_DATA
            | PackageManager.GET_CONFIGURATIONS | PackageManager.GET_PERMISSIONS
            | PackageManager.GET_SHARED_LIBRARY_FILES;

    /** Both kinds of components, as PackageManagerService adds when neither is asked for. */
    private static final long DIRECT_BOOT = PackageManager.MATCH_DIRECT_BOOT_AWARE
            | PackageManager.MATCH_DIRECT_BOOT_UNAWARE;

    public final String apkPath;
    public final ParsedPackage parsed;
    public final String packageName;
    public final SharedLibraryGraph sharedLibraries;
    private final List<String> mLauncherActivities = new ArrayList<>();
    private long mInstallTime = System.currentTimeMillis();
    private long mUpdateTime = mInstallTime;
    private String mDataDir;
    private AppDataStorage mDataStorage;
    private String mNativeLibraryDir;  // null: <data dir>/lib
    private String mPrimaryCpuAbi;     // null: no native code (NativeLibs.abiFor)
    private final boolean mHasCode;
    private Boolean mVerifiedMicrogSigner;
    private final int mUid;
    private boolean mSystemAddon;
    private boolean mSigningIdentityChecked;
    private Signature[] mVerifiedSignatures;
    private int mVerifiedSignatureSchemeVersion;
    private com.android.apksig.SigningCertificateLineage mVerifiedLineage;
    // A parsed record can be used before a package service owns it (standalone tools).
    // Installed records all read the service's single atomic enabled-state snapshot.
    private volatile PackageEnabledStore.State mLocalEnabledState = PackageEnabledStore.State.DEFAULT;
    private volatile PackageEnabledStore mEnabledStore;
    private String mEnabledOwner;
    // Only a committed non-DONT_KILL_APP change retires this process generation.
    // Component cleanup itself follows AOSP's current enabled state at dispatch.
    private EnabledProcess mEnabledProcess;
    private static final class EnabledProcess implements java.util.function.BooleanSupplier {
        volatile boolean current = true;
        @Override public boolean getAsBoolean() { return current; }
    }

    synchronized java.util.function.BooleanSupplier enabledProcessLifetime() {
        if (mEnabledProcess == null) mEnabledProcess = new EnabledProcess();
        return mEnabledProcess;
    }

    synchronized void retireEnabledProcesses() {
        if (mEnabledProcess != null) mEnabledProcess.current = false;
        mEnabledProcess = null;
    }

    void attachEnabledState(PackageEnabledStore store, String owner) {
        mEnabledOwner = owner; mEnabledStore = store;
    }
    boolean hasEnabledState(PackageEnabledStore store) {
        return mEnabledStore == store && mEnabledOwner != null;
    }
    String enabledStateOwner() { return mEnabledOwner; }
    PackageEnabledStore.State enabledState() {
        PackageEnabledStore store = mEnabledStore;
        if (store != null) return store.state(packageName, mEnabledOwner);
        return mLocalEnabledState == null ? PackageEnabledStore.State.DEFAULT : mLocalEnabledState;
    }
    int applicationEnabledState() { return enabledState().application(); }

    int componentEnabledState(String name) {
        return enabledState().component(name);
    }

    synchronized void setComponentEnabledState(String name, int state) {
        PackageEnabledStore.State next = enabledState().component(name, state);
        if (mEnabledStore == null) mLocalEnabledState = next;
        else mEnabledStore.commit(Map.of(packageName, new PackageEnabledStore.Entry(mEnabledOwner, next)));
    }

    void clearComponentStates() {
        retireEnabledProcesses();
        mEnabledStore = null; mEnabledOwner = null;
        mLocalEnabledState = PackageEnabledStore.State.DEFAULT;
    }

    boolean matchesEnabledApplication(long flags) {
        return matchesEnabledApplication(enabledState(), flags);
    }

    enum ComponentKind { ACTIVITY, SERVICE, RECEIVER, PROVIDER }

    /** Admission after an asynchronous wait must use today's installed state and family. */
    boolean componentEnabled(String name, ComponentKind kind) {
        PackageEnabledStore.State state = enabledState();
        if (!matchesEnabledApplication(state, 0)) return false;
        List<? extends ParsedMainComponent> declarations = switch (kind) {
            case ACTIVITY -> parsed.getActivities();
            case SERVICE -> parsed.getServices();
            case RECEIVER -> parsed.getReceivers();
            case PROVIDER -> parsed.getProviders();
        };
        for (ParsedMainComponent component : declarations) {
            if (!component.getName().equals(name)) continue;
            return switch (state.component(name)) {
                case PackageManager.COMPONENT_ENABLED_STATE_ENABLED -> true;
                case PackageManager.COMPONENT_ENABLED_STATE_DISABLED -> false;
                default -> component.isEnabled();
            };
        }
        return false;
    }
    private boolean matchesEnabledApplication(PackageEnabledStore.State state, long flags) {
        if ((flags & PackageManager.MATCH_DISABLED_COMPONENTS) != 0) return true;
        return switch (state.application()) {
            case PackageManager.COMPONENT_ENABLED_STATE_ENABLED -> true;
            case PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                    PackageManager.COMPONENT_ENABLED_STATE_DISABLED_USER -> false;
            case PackageManager.COMPONENT_ENABLED_STATE_DISABLED_UNTIL_USED ->
                    (flags & PackageManager.MATCH_DISABLED_UNTIL_USED_COMPONENTS) != 0 && parsed.isEnabled();
            default -> parsed.isEnabled();
        };
    }

    private ApkPackage(String apkPath, ParsedPackage parsed) throws IOException {
        this.apkPath = apkPath;
        this.parsed = parsed;
        this.packageName = parsed.getPackageName();
        if ("android".equals(packageName)) {
            throw new SecurityException("The platform package name is reserved");
        }
        mUid = PackageUids.uidFor(packageName);
        // Resolve the newly parsed APK, including before an install replaces the old one.
        sharedLibraries = SharedLibraryRegistry.resolve(parsed);
        mHasCode = parsed.isDeclaredHavingCode() && hasDex(apkPath);
        for (ParsedActivity a : parsed.getActivities()) {
            for (ParsedIntentInfo info : a.getIntents()) {
                IntentFilter filter = info.getIntentFilter();
                if (filter.hasAction(Intent.ACTION_MAIN)
                        && filter.hasCategory(Intent.CATEGORY_LAUNCHER)) {
                    mLauncherActivities.add(a.getName());
                    break;
                }
            }
        }
        mDataStorage = AppDataStorage.installed(new File(Device.dataDir()), packageName);
        mDataDir = mDataStorage.credential.toString();
    }

    /** PackageParser2's callback outside system_server: no compat change is enabled. */
    private static final class ParserCallback extends PackageParser2.Callback {
        @Override
        public boolean isChangeEnabled(long changeId, ApplicationInfo appInfo) {
            return false;
        }

        @Override
        public boolean hasFeature(String feature) {
            return false;
        }

        @Override
        public Set<String> getHiddenApiWhitelistedApps() {
            return Collections.emptySet();
        }

        @Override
        public Set<String> getInstallConstraintsAllowlist() {
            return Collections.emptySet();
        }
    }

    /** Parses one APK; unrelated files beside an external APK are never part of it. */
    public static ApkPackage parse(String apkPath) throws Exception {
        return parse(apkPath, new File(apkPath));
    }

    /**
     * Parses a complete, installer-owned code generation. PackageParser2's cluster path
     * merges feature manifests and builds the manifest-named split dependency graph;
     * merely appending sibling paths to ApplicationInfo cannot load isolated features.
     */
    public static ApkPackage parseCodeDirectory(String codePath) throws Exception {
        File directory = new File(codePath).getAbsoluteFile();
        File appRoot = new File(new File(Device.dataDir()).getCanonicalFile(), "app");
        File canonicalDirectory = directory.getCanonicalFile();
        if (!appRoot.equals(directory.getParentFile())
                || !directory.equals(canonicalDirectory)
                || !Files.isDirectory(directory.toPath(), LinkOption.NOFOLLOW_LINKS)) {
            throw new SecurityException("Package code is outside the installed APK directory: "
                    + directory + "; canonical=" + canonicalDirectory + "; expected parent=" + appRoot);
        }
        File base = new File(directory, "base.apk");
        ApkLite baseLite = codeApkLite(base);
        if (baseLite.getSplitName() != null
                || !directory.getName().matches(java.util.regex.Pattern.quote(baseLite.getPackageName())
                        + "-[0-9]+")) {
            throw new IOException("Invalid installed base APK: " + base);
        }
        File[] apks = directory.listFiles((dir, name) -> name.endsWith(".apk"));
        if (apks == null) throw new IOException("Cannot read package code directory: " + directory);
        Set<String> requiredTypes = new HashSet<>();
        Set<String> splitTypes = new HashSet<>();
        for (File file : apks) {
            boolean isBase = file.equals(base);
            ApkLite split = isBase ? baseLite : codeApkLite(file);
            // Include versionCodeMajor: the framework's cluster-lite check only compares
            // the low versionCode, whereas an installed set must have one long version.
            if (!isBase && (!file.getName().startsWith("split_") || split.getSplitName() == null
                    || !baseLite.getPackageName().equals(split.getPackageName())
                    || baseLite.getLongVersionCode() != split.getLongVersionCode())) {
                throw new IOException("Inconsistent installed split APK: " + file);
            }
            if (split.getRequiredSplitTypes() != null) requiredTypes.addAll(split.getRequiredSplitTypes());
            if (split.getSplitTypes() != null) splitTypes.addAll(split.getSplitTypes());
        }
        // PackageInstallerSession enforces this before publishing a full APK set;
        // PackageParser2's cluster parser alone does not validate required split types.
        if (baseLite.isSplitRequired() && (apks.length <= 1 || !splitTypes.containsAll(requiredTypes))) {
            throw new android.content.pm.PackageParser.PackageParserException(
                    PackageManager.INSTALL_FAILED_MISSING_SPLIT,
                    "INSTALL_FAILED_MISSING_SPLIT: " + baseLite.getPackageName());
        }
        return parse(base.getPath(), directory);
    }

    private static ApkLite codeApkLite(File file) throws IOException {
        if (!Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS)) {
            throw new IOException("Not a regular installed APK: " + file);
        }
        ParseResult<ApkLite> result = ApkLiteParseUtils.parseApkLite(
                ParseTypeImpl.forParsingWithoutPlatformCompat(), file, 0);
        if (result.isError()) {
            throw new IOException("Cannot parse installed APK " + file + ": "
                    + result.getErrorMessage(), result.getException());
        }
        return result.getResult();
    }

    private static ApkPackage parse(String apkPath, File parseRoot) throws Exception {
        ParsedPackage pkg;
        try (PackageParser2 parser = new PackageParser2(null /* separateProcesses */,
                null /* displayMetrics: the defaults */, null /* cacher */,
                new ParserCallback())) {
            pkg = parser.parsePackage(parseRoot, 0 /* flags */, false /* useCaches */);
        }
        SigningDetails signing = signingDetails(apkPath);
        if (signing != null) {
            pkg.setSigningDetails(signing);
        }
        ApkPackage appPkg = new ApkPackage(apkPath, pkg);
        if (pkg.getSigningDetails() == null || pkg.getSigningDetails() == SigningDetails.UNKNOWN) {
            Signature[] verified = appPkg.verifiedSignatures();
            if (verified != null) {
                pkg.setSigningDetails(new SigningDetails(verified,
                        appPkg.mVerifiedSignatureSchemeVersion));
            }
        }
        return appPkg;
    }

    /**
     * The APK's signers from its v3 or v2 signing block, unverified, as ApkSignatureVerifier
     * collects them (with v3's proof-of-rotation: GET_SIGNATURES answers the oldest key of a
     * rotated one); null (no signatures) for an unsigned or v1-only APK. Not
     * ApkSignatureVerifier itself: it tries v4 first, through IncrementalManager's native
     * code, which Gesher does not have.
     */
    private static SigningDetails signingDetails(String apkPath) {
        try {
            try {
                ApkSignatureSchemeV3Verifier.VerifiedSigner v3 =
                        ApkSignatureSchemeV3Verifier.unsafeGetCertsWithoutVerification(apkPath);
                Signature[] past = null;
                if (v3.por != null) {
                    past = new Signature[v3.por.certs.size()];
                    for (int i = 0; i < past.length; i++) {
                        past[i] = new Signature(v3.por.certs.get(i).getEncoded());
                        past[i].setFlags(v3.por.flagsList.get(i));
                    }
                }
                return new SigningDetails(signatures(new X509Certificate[][] {v3.certs}),
                        SigningDetails.SignatureSchemeVersion.SIGNING_BLOCK_V3, past);
            } catch (SignatureNotFoundException e) {
                // Not v3: try v2.
            }
            return new SigningDetails(signatures(
                    ApkSignatureSchemeV2Verifier.unsafeGetCertsWithoutVerification(apkPath)),
                    SigningDetails.SignatureSchemeVersion.SIGNING_BLOCK_V2);
        } catch (Throwable e) {
            return null;
        }
    }

    /** One Signature per signer, with its certificate chain (convertToSignatures). */
    private static Signature[] signatures(X509Certificate[][] signers) throws Exception {
        Signature[] signatures = new Signature[signers.length];
        for (int i = 0; i < signers.length; i++) {
            signatures[i] = new Signature(signers[i]);
        }
        return signatures;
    }

    /** One line about what was parsed, for logs (and the tests' checks). */
    public String describe() {
        int activities = 0;
        for (ParsedActivity a : parsed.getActivities()) {
            // The parser adds the system's AppDetailsActivity to every package.
            if (!PackageManager.APP_DETAILS_ACTIVITY_CLASS_NAME.equals(a.getName())) {
                activities++;
            }
        }
        return packageName + " parsed by PackageParser2: "
                + activities + " activities, "
                + parsed.getServices().size() + " services, "
                + parsed.getReceivers().size() + " receivers, "
                + parsed.getProviders().size() + " providers, targetSdk "
                + parsed.getTargetSdkVersion();
    }

    /** An explicit CE directory override; its separate DE directory is a sibling. */
    public void setDataDir(String dataDir) {
        mDataStorage = AppDataStorage.custom(dataDir);
        mDataDir = mDataStorage.credential.toString();
    }

    /** AOSP ScanPackageUtils permits this manifest default only for system packages. */
    private boolean defaultsToDeviceProtectedStorage() {
        return mSystemAddon && parsed.isDefaultToDeviceProtectedStorage();
    }

    /** The effective installed default area after system-package policy is applied. */
    public String dataDir() {
        return defaultsToDeviceProtectedStorage() ? deviceProtectedDataDir() : mDataDir;
    }

    public String credentialProtectedDataDir() {
        return mDataDir;
    }

    public String deviceProtectedDataDir() {
        return mDataStorage != null ? mDataStorage.device.toString()
                : mDataDir == null ? null : AppDataStorage.custom(mDataDir).device.toString();
    }

    /** Prepare a launch/new install, without guessing which manifest created legacy data. */
    public void prepareDataDirectories() throws IOException {
        storage().prepare(defaultsToDeviceProtectedStorage(), false);
    }

    /** Only the already committed installed manifest may authorize legacy default-DE migration. */
    void prepareInstalledDataDirectories() throws IOException {
        storage().prepare(defaultsToDeviceProtectedStorage(), true);
    }

    void prepareInstallDataDirectories(boolean replacing) throws IOException {
        if (replacing) storage().prepareUpdate(defaultsToDeviceProtectedStorage());
        else prepareDataDirectories();
    }

    private AppDataStorage storage() {
        return mDataStorage != null ? mDataStorage : AppDataStorage.custom(mDataDir);
    }

    /**
     * Where the package's native libraries are (an installed package's
     * {@code <code path>/lib/x86_64}); by default {@code <data dir>/lib}.
     */
    public void setNativeLibraryDir(String dir) {
        mNativeLibraryDir = dir;
    }

    /** The ABI of the package's native code (x86_64, arm64-v8a), or null if it has none. */
    public void setPrimaryCpuAbi(String abi) {
        mPrimaryCpuAbi = abi;
    }

    public String nativeLibraryDir() {
        return mNativeLibraryDir != null ? mNativeLibraryDir : mDataDir + "/lib";
    }

    /** When it was first installed and last updated (PackageInfo's times). */
    public void setInstallTimes(long firstInstallTime, long lastUpdateTime) {
        mInstallTime = firstInstallTime;
        mUpdateTime = lastUpdateTime;
    }

    public long firstInstallTime() {
        return mInstallTime;
    }

    public long versionCode() {
        return parsed.getLongVersionCode();
    }

    public int uid() { return mUid; }
    void setSystemAddon(boolean enabled) {
        if (enabled) {
            Signature[] verified = verifiedSignatures();
            if (verified == null) throw new SecurityException("Unverified system addon");
            // The pinned GSF proxy uses v1 only; the compatibility v2/v3 collector
            // above cannot supply its real package signature. Never substitute it.
            if (parsed.getSigningDetails() == null
                    || parsed.getSigningDetails() == SigningDetails.UNKNOWN) {
                try {
                    parsed.setSigningDetails(new SigningDetails(verified,
                            mVerifiedSignatureSchemeVersion));
                } catch (java.security.cert.CertificateException failure) {
                    throw new SecurityException("Invalid verified signer certificate", failure);
                }
            }
        }
        mSystemAddon = enabled;
    }
    public boolean isSystemAddon() { return mSystemAddon; }

    /** Current APK signers after full content verification, never the spoofed identity. */
    public synchronized Signature[] verifiedSignatures() {
        if (!mSigningIdentityChecked) {
            mSigningIdentityChecked = true;
            VerifiedIdentity identity = verifyIdentity(apkPath);
            if (identity != null) {
                mVerifiedSignatures = identity.signatures;
                mVerifiedLineage = identity.lineage;
                mVerifiedSignatureSchemeVersion = identity.signatureSchemeVersion;
            }
        }
        return mVerifiedSignatures == null ? null : mVerifiedSignatures.clone();
    }

    /** Matches a SHA-256 certificate digest against the verified APK identity. */
    synchronized boolean hasVerifiedSigningCertificate(byte[] certificate) {
        Signature[] current = verifiedSignatures();
        if (current == null) return false;
        final MessageDigest digest;
        try {
            digest = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
        for (Signature signature : current) {
            if (MessageDigest.isEqual(certificate, digest.digest(signature.toByteArray()))) return true;
        }
        // Like Android, only a lone current signer can have signing history.
        if (current.length != 1 || mVerifiedLineage == null) return false;
        try {
            List<X509Certificate> history = mVerifiedLineage.getCertificatesInLineage();
            if (history.isEmpty() || !current[0].equals(new Signature(
                    history.get(history.size() - 1).getEncoded()))) return false;
            for (int i = 0; i < history.size() - 1; i++) {
                if (MessageDigest.isEqual(certificate, digest.digest(history.get(i).getEncoded()))) {
                    return true;
                }
            }
        } catch (CertificateEncodingException invalid) {
            return false;
        }
        return false;
    }

    public static Signature[] verifySignatures(String path) {
        VerifiedIdentity identity = verifyIdentity(path);
        return identity == null ? null : identity.signatures.clone();
    }

    private static final class VerifiedIdentity {
        final Signature[] signatures;
        final com.android.apksig.SigningCertificateLineage lineage;
        final int signatureSchemeVersion;
        VerifiedIdentity(Signature[] signatures, com.android.apksig.SigningCertificateLineage lineage,
                int signatureSchemeVersion) {
            this.signatures = signatures;
            this.lineage = lineage;
            this.signatureSchemeVersion = signatureSchemeVersion;
        }
    }

    private static VerifiedIdentity verifyIdentity(String path) {
        try {
            com.android.apksig.ApkVerifier.Result result = ApkSigningVerifier.verify(new File(path));
            if (result.isVerified() && !result.getSignerCertificates().isEmpty()) {
                Signature[] signatures = new Signature[result.getSignerCertificates().size()];
                for (int i = 0; i < signatures.length; i++) {
                    signatures[i] = new Signature(result.getSignerCertificates().get(i).getEncoded());
                }
                Arrays.sort(signatures, java.util.Comparator.comparing(Signature::toCharsString));
                // Only lineage returned by the successful full APK verifier is trusted.
                // The compatibility parser's unsafe certificate/proof collection is not.
                int scheme = result.isVerifiedUsingV31Scheme() || result.isVerifiedUsingV3Scheme()
                        ? SigningDetails.SignatureSchemeVersion.SIGNING_BLOCK_V3
                        : result.isVerifiedUsingV2Scheme()
                                ? SigningDetails.SignatureSchemeVersion.SIGNING_BLOCK_V2
                                : SigningDetails.SignatureSchemeVersion.JAR;
                return new VerifiedIdentity(signatures, result.getSigningCertificateLineage(), scheme);
            }
        } catch (Exception | LinkageError failure) {
            SystemServer.log("APK signing identity verification failed: "
                    + failure.getClass().getSimpleName());
        }
        return null;
    }

    public boolean hasVerifiedSigningIdentity() { return verifiedSignatures() != null; }

    public String verifiedSignerSha256() {
        Signature[] signatures = verifiedSignatures();
        if (signatures == null || signatures.length != 1) return null;
        return certificateSha256(signatures[0].toByteArray());
    }

    private static String certificateSha256(byte[] certificate) {
        try {
            byte[] digest = java.security.MessageDigest.getInstance("SHA-256")
                    .digest(certificate);
            StringBuilder hex = new StringBuilder(64);
            for (byte b : digest) {
                hex.append(Character.forDigit((b >>> 4) & 15, 16));
                hex.append(Character.forDigit(b & 15, 16));
            }
            return hex.toString();
        } catch (java.security.NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
    }

    public boolean signaturesMatch(ApkPackage other) {
        Signature[] current = verifiedSignatures();
        Signature[] theirs = other == null ? null : other.verifiedSignatures();
        return current != null && theirs != null && Arrays.equals(current, theirs);
    }

    /**
     * Installation continuity is broader than cross-package signature permission
     * equality: a verified forward key rotation may retain the predecessor's data.
     * Rollback, unrelated keys and multi-signer set changes remain rejected.
     */
    public boolean updateContinuityFrom(ApkPackage previous) {
        if (previous == null || !packageName.equals(previous.packageName)) return false;
        Signature[] current = verifiedSignatures();
        Signature[] before = previous.verifiedSignatures();
        if (current == null || before == null) return false;
        if (Arrays.equals(current, before)) return true;
        if (current.length != 1 || before.length != 1) return false;
        com.android.apksig.SigningCertificateLineage lineage;
        synchronized (this) { lineage = mVerifiedLineage; }
        if (lineage == null) return false;
        try {
            List<X509Certificate> certificates = lineage.getCertificatesInLineage();
            if (certificates.size() < 2 || !current[0].equals(new Signature(
                    certificates.get(certificates.size() - 1).getEncoded()))) return false;
            for (int i = 0; i + 1 < certificates.size(); i++) {
                X509Certificate ancestor = certificates.get(i);
                if (before[0].equals(new Signature(ancestor.getEncoded()))) {
                    return lineage.getSignerCapabilities(ancestor).hasInstalledData();
                }
            }
        } catch (Exception failure) {
            return false;
        }
        return false;
    }

    /** Oldest verified AUTH-retained ancestor; current-key fingerprints stay separate. */
    public String verifiedAuthSignerSha256() {
        Signature[] current = verifiedSignatures();
        if (current == null || current.length != 1) return null;
        com.android.apksig.SigningCertificateLineage lineage;
        synchronized (this) { lineage = mVerifiedLineage; }
        if (lineage != null) {
            try {
                List<X509Certificate> certificates = lineage.getCertificatesInLineage();
                if (!certificates.isEmpty() && current[0].equals(new Signature(
                        certificates.get(certificates.size() - 1).getEncoded()))) {
                    for (int i = 0; i + 1 < certificates.size(); i++) {
                        X509Certificate ancestor = certificates.get(i);
                        if (lineage.getSignerCapabilities(ancestor).hasAuth()) {
                            return certificateSha256(ancestor.getEncoded());
                        }
                    }
                }
            } catch (Exception ignored) {
                // A valid current key does not inherit any unproven AUTH capability.
            }
        }
        return certificateSha256(current[0].toByteArray());
    }

    /** Full APK integrity verification, separate from our compatibility cert collection. */
    synchronized boolean hasVerifiedMicrogSigner() {
        if (mVerifiedMicrogSigner == null) {
            mVerifiedMicrogSigner = GmsSignatureSpoofing.verifyOfficialApk(apkPath);
        }
        return mVerifiedMicrogSigner;
    }

    /** The PackageInfo PackageManager.getPackageInfo answers for these flags. */
    public PackageInfo packageInfo(long flags) {
        if ((flags & DIRECT_BOOT) == 0) {
            flags |= DIRECT_BOOT;
        }
        PackageEnabledStore.State state = enabledState();
        // Include manifest-disabled declarations before applying installed overrides.
        PackageInfo pi = PackageInfoCommonUtils.generate(parsed,
                flags | PackageManager.MATCH_DISABLED_COMPONENTS, 0 /* userId */);
        installed(pi.applicationInfo, state, flags);
        pi.activities = installedComponents(pi.activities, pi.applicationInfo, flags, state);
        pi.receivers = installedComponents(pi.receivers, pi.applicationInfo, flags, state);
        pi.services = installedComponents(pi.services, pi.applicationInfo, flags, state);
        pi.providers = installedComponents(pi.providers, pi.applicationInfo, flags, state);
        pi.firstInstallTime = mInstallTime;
        pi.lastUpdateTime = mUpdateTime;
        return GmsSignatureSpoofing.maybeSpoofPackageInfo(this, pi, flags);
    }

    /** Apply installed state once, including provider startup and activity-options queries. */
    private <T extends ComponentInfo> T[] installedComponents(T[] components,
            ApplicationInfo application, long flags, PackageEnabledStore.State state) {
        if (components == null) return null;
        boolean includeDisabled = (flags & PackageManager.MATCH_DISABLED_COMPONENTS) != 0;
        boolean applicationMatches = matchesEnabledApplication(state, flags);
        int count = 0;
        for (T info : components) {
            component(info, application);
            int componentState = state.component(info.name);
            if (componentState != PackageManager.COMPONENT_ENABLED_STATE_DEFAULT) {
                info.enabled = componentState == PackageManager.COMPONENT_ENABLED_STATE_ENABLED;
            }
            if (includeDisabled || applicationMatches && info.enabled) components[count++] = info;
        }
        return count == components.length ? components
                : count == 0 ? null : Arrays.copyOf(components, count);
    }

    public ApplicationInfo applicationInfo(long flags) {
        return packageInfo(flags & ~(PackageManager.GET_ACTIVITIES
                | PackageManager.GET_RECEIVERS | PackageManager.GET_SERVICES
                | PackageManager.GET_PROVIDERS | PackageManager.GET_PERMISSIONS
                | PackageManager.GET_CONFIGURATIONS)).applicationInfo;
    }

    /** The activity named {@code className}, or null. */
    public ActivityInfo activityInfo(String className, long flags) {
        PackageInfo pi = packageInfo(flags | PackageManager.GET_ACTIVITIES);
        if (pi.activities != null) {
            for (ActivityInfo a : pi.activities) {
                if (a.name.equals(className)) {
                    return a;
                }
            }
        }
        return null;
    }

    /** The Activity to launch: {@code requested}, or the first enabled MAIN/LAUNCHER one. */
    public ActivityInfo launcherActivity(String requested) {
        if (requested == null) {
            // A disabled launcher/alias can precede the app's active icon. Use one
            // installed-state metadata snapshot and preserve manifest launcher order.
            ActivityInfo[] activities = packageInfo(PackageManager.GET_ACTIVITIES).activities;
            if (activities != null) {
                for (String name : mLauncherActivities) {
                    for (ActivityInfo activity : activities) {
                        if (name.equals(activity.name)) return activity;
                    }
                }
            }
            throw new IllegalStateException(apkPath + " has no enabled MAIN/LAUNCHER activity;"
                    + " name one with --activity");
        }
        String name = qualify(requested);
        ActivityInfo a = activityInfo(name, 0);
        if (a == null) {
            throw new IllegalStateException(apkPath + " declares no activity " + name);
        }
        return a;
    }

    private String qualify(String name) {
        if (name.startsWith(".")) {
            return packageName + name;
        }
        if (name.indexOf('.') < 0) {
            return packageName + "." + name;
        }
        return name;
    }

    /**
     * The activities whose intent filters match {@code intent}, as
     * PackageManager.queryIntentActivities answers (an explicit component matches itself).
     */
    public List<ResolveInfo> queryIntentActivities(Intent intent, String resolvedType,
            long flags) {
        List<ResolveInfo> result = new ArrayList<>();
        if (intent.getPackage() != null && !intent.getPackage().equals(packageName)) {
            return result;
        }
        ComponentName component = intent.getComponent();
        if (component != null) {
            if (component.getPackageName().equals(packageName)) {
                ActivityInfo a = activityInfo(component.getClassName(), flags);
                if (a != null) {
                    ResolveInfo ri = new ResolveInfo();
                    ri.activityInfo = a;
                    result.add(ri);
                }
            }
            return result;
        }
        for (Match m : match(parsed.getActivities(), intent, resolvedType, flags)) {
            ActivityInfo a = activityInfo(m.name, flags);
            if (a != null) result.add(m.resolveInfo(flags, a, null));
        }
        return result;
    }

    /**
     * The services whose intent filters match {@code intent} (an explicit component matches
     * itself), as PackageManager.queryIntentServices answers.
     */
    public List<ResolveInfo> queryIntentServices(Intent intent, String resolvedType,
            long flags) {
        List<ResolveInfo> result = new ArrayList<>();
        if (intent.getPackage() != null && !intent.getPackage().equals(packageName)) {
            return result;
        }
        ComponentName component = intent.getComponent();
        if (component != null) {
            ServiceInfo s = component.getPackageName().equals(packageName)
                    ? serviceInfo(component.getClassName(), flags) : null;
            if (s != null) {
                ResolveInfo ri = new ResolveInfo();
                ri.serviceInfo = s;
                result.add(ri);
            }
            return result;
        }
        for (Match m : match(parsed.getServices(), intent, resolvedType, flags)) {
            ServiceInfo s = serviceInfo(m.name, flags);
            if (s != null) result.add(m.resolveInfo(flags, null, s));
        }
        return result;
    }

    /**
     * The manifest receivers {@code intent} reaches: the explicit component, or the ones
     * whose intent filters match it (PackageManager.queryBroadcastReceivers).
     */
    public List<ResolveInfo> queryIntentReceivers(Intent intent, String resolvedType,
            long flags) {
        List<ResolveInfo> result = new ArrayList<>();
        if (intent.getPackage() != null && !intent.getPackage().equals(packageName)) {
            return result;
        }
        ComponentName component = intent.getComponent();
        if (component != null) {
            ActivityInfo a = component.getPackageName().equals(packageName)
                    ? receiverInfo(component.getClassName(), flags) : null;
            if (a != null) {
                ResolveInfo ri = new ResolveInfo();
                ri.activityInfo = a;
                result.add(ri);
            }
            return result;
        }
        for (Match m : match(parsed.getReceivers(), intent, resolvedType, flags)) {
            ActivityInfo a = receiverInfo(m.name, flags);
            if (a != null) result.add(m.resolveInfo(flags, a, null));
        }
        return result;
    }

    /** The service named {@code className}, or null. */
    public ServiceInfo serviceInfo(String className, long flags) {
        PackageInfo pi = packageInfo(flags | PackageManager.GET_SERVICES);
        if (pi.services != null) {
            for (ServiceInfo s : pi.services) {
                if (s.name.equals(className)) return s;
            }
        }
        return null;
    }

    /** The manifest receiver named {@code className}, or null. */
    public ActivityInfo receiverInfo(String className, long flags) {
        PackageInfo pi = packageInfo(flags | PackageManager.GET_RECEIVERS);
        if (pi.receivers != null) {
            for (ActivityInfo a : pi.receivers) {
                if (a.name.equals(className)) return a;
            }
        }
        return null;
    }

    /**
     * The content providers whose intent filters match {@code intent}
     * (PackageManager.queryIntentContentProviders: DocumentsProvider discovery, for one).
     */
    public List<ResolveInfo> queryIntentContentProviders(Intent intent, String resolvedType,
            long flags) {
        List<ResolveInfo> result = new ArrayList<>();
        if (intent.getPackage() != null && !intent.getPackage().equals(packageName)) {
            return result;
        }
        ComponentName component = intent.getComponent();
        List<ProviderInfo> providers = providers(flags);
        if (component != null) {
            if (component.getPackageName().equals(packageName)) {
                for (ProviderInfo p : providers) {
                    if (p.name.equals(component.getClassName())) {
                        ResolveInfo ri = new ResolveInfo();
                        ri.providerInfo = p;
                        result.add(ri);
                    }
                }
            }
            return result;
        }
        for (Match m : match(parsed.getProviders(), intent, resolvedType, flags)) {
            for (ProviderInfo p : providers) {
                if (!p.name.equals(m.name)) continue;
                ResolveInfo ri = m.resolveInfo(flags, null, null);
                ri.providerInfo = p;
                result.add(ri);
                break;
            }
        }
        return result;
    }

    /** The content providers the manifest declares. */
    public List<ProviderInfo> providers(long flags) {
        PackageInfo pi = packageInfo(flags | PackageManager.GET_PROVIDERS);
        List<ProviderInfo> result = new ArrayList<>();
        if (pi.providers != null) Collections.addAll(result, pi.providers);
        return result;
    }

    /** A component whose intent filter matched, and how. */
    private static final class Match {
        final String name;
        final IntentFilter filter;
        final ParsedIntentInfo info;
        final int match;

        Match(String name, IntentFilter filter, ParsedIntentInfo info, int match) {
            this.name = name;
            this.filter = filter;
            this.info = info;
            this.match = match;
        }

        ResolveInfo resolveInfo(long flags, ActivityInfo activity, ServiceInfo service) {
            ResolveInfo ri = new ResolveInfo();
            ri.activityInfo = activity;
            ri.serviceInfo = service;
            if ((flags & PackageManager.GET_RESOLVED_FILTER) != 0) {
                ri.filter = new IntentFilter(filter);
            }
            ri.match = match;
            ri.priority = filter.getPriority();
            ri.isDefault = filter.hasCategory(Intent.CATEGORY_DEFAULT);
            ri.labelRes = info.getLabelRes();
            ri.icon = info.getIcon();
            return ri;
        }
    }

    /**
     * The components whose intent filters match {@code intent}, the first matching filter of
     * each, as PackageManagerService's IntentResolver matches them ({@code
     * MATCH_DEFAULT_ONLY}: only filters with CATEGORY_DEFAULT).
     */
    private static List<Match> match(List<? extends ParsedComponent> components,
            Intent intent, String resolvedType, long flags) {
        List<Match> result = new ArrayList<>();
        String type = resolvedType != null ? resolvedType : intent.getType();
        boolean defaultOnly = (flags & PackageManager.MATCH_DEFAULT_ONLY) != 0;
        for (ParsedComponent c : components) {
            for (ParsedIntentInfo info : c.getIntents()) {
                IntentFilter filter = info.getIntentFilter();
                if (defaultOnly && !filter.hasCategory(Intent.CATEGORY_DEFAULT)) continue;
                int match = filter.match(intent.getAction(), type, intent.getScheme(),
                        intent.getData(), intent.getCategories(), "gesher");
                if (match < 0) continue;
                result.add(new Match(c.getName(), filter, info, match));
                break;
            }
        }
        return result;
    }

    /** What installing gives a package beyond its manifest: paths, uid, data, enabled. */
    private void installed(ApplicationInfo ai, PackageEnabledStore.State state, long flags) {
        ai.sourceDir = apkPath;
        ai.publicSourceDir = apkPath;
        if (ai.processName == null) {
            ai.processName = packageName;
        }
        ai.uid = mUid;
        ai.enabledSetting = state.application();
        ai.enabled = state.application() == PackageManager.COMPONENT_ENABLED_STATE_ENABLED
                || state.application() == PackageManager.COMPONENT_ENABLED_STATE_DEFAULT && parsed.isEnabled()
                || state.application() == PackageManager.COMPONENT_ENABLED_STATE_DISABLED_UNTIL_USED
                    && (flags & PackageManager.MATCH_DISABLED_UNTIL_USED_COMPONENTS) != 0;
        // PackageParser2 returns a ParsedPackage, before PackageImpl.hideAsFinal
        // fills its cached ApplicationInfo flags. Derive all manifest flags using
        // the same AOSP helpers; restoring individual flags leaves heap, network,
        // native-library and storage policy absent from ordinary app initialization.
        // Keep the installed/state flags generated above and our actual-DEX gate.
        ai.flags = (ai.flags | AppInfoUtils.appInfoFlags(parsed))
                & ~ApplicationInfo.FLAG_HAS_CODE;
        ai.privateFlags |= AppInfoUtils.appInfoPrivateFlags(parsed);
        if (!defaultsToDeviceProtectedStorage()) {
            // Parsed manifest flags have not passed AOSP's system-package scan policy.
            // The published default flag and all directory preparation must agree.
            ai.privateFlags &= ~ApplicationInfo.PRIVATE_FLAG_DEFAULT_TO_DEVICE_PROTECTED_STORAGE;
        }
        ai.flags |= ApplicationInfo.FLAG_INSTALLED;
        if (mSystemAddon) {
            ai.flags |= ApplicationInfo.FLAG_SYSTEM;
            ai.privateFlags |= ApplicationInfo.PRIVATE_FLAG_PRIVILEGED;
        }
        if (mHasCode) {
            // As installed packages carry it: LoadedApk loads code only with it.
            ai.flags |= ApplicationInfo.FLAG_HAS_CODE;
        }
        // privateFlagsExt comes from PackageImpl's mBaseAppInfoPrivateFlagsExt, also set only
        // in hideAsFinal (assignDerivedFields2), so it was 0 here: an application's
        // android:enableOnBackInvokedCallback="true" (PRIVATE_FLAG_EXT_ENABLE_ON_BACK_INVOKED_CALLBACK)
        // was dropped and WindowOnBackInvokedDispatcher logged "OnBackInvokedCallback is not
        // enabled for the application". The same derivation, from the parsed manifest
        // (the attributions bit is left as PackageInfoCommonUtils.generate set it for the flags).
        ai.privateFlagsExt |= AppInfoUtils.appInfoPrivateFlagsExt(parsed,
                false /* not allowlisted for hidden APIs */)
                & ~ApplicationInfo.PRIVATE_FLAG_EXT_ATTRIBUTIONS_ARE_USER_VISIBLE;
        ai.dataDir = dataDir();
        ai.credentialProtectedDataDir = credentialProtectedDataDir();
        ai.deviceProtectedDataDir = deviceProtectedDataDir();
        ai.nativeLibraryDir = nativeLibraryDir();
        ai.primaryCpuAbi = mPrimaryCpuAbi;

        // A long-lived PM record must not publish a cached graph after XML/jar updates.
        // Ordinary packages keep the cheap immutable empty graph path.
        try {
            SharedLibraryGraph graph = SharedLibraryRegistry.usesNone(parsed) ? sharedLibraries
                    : SharedLibraryRegistry.resolve(parsed);
            graph.apply(ai);
        } catch (IOException invalidLibrary) {
            throw new IllegalStateException("shared libraries changed for " + packageName, invalidLibrary);
        }
    }

    private static void component(ComponentInfo c, ApplicationInfo ai) {
        c.applicationInfo = ai;
        c.packageName = ai.packageName;
        if (c.processName == null) {
            c.processName = ai.processName;
        }
    }

    /** Whether the APK carries code: a classes.dex. */
    private static boolean hasDex(String apkPath) {
        try (java.util.zip.ZipFile zip = new java.util.zip.ZipFile(apkPath)) {
            return zip.getEntry("classes.dex") != null;
        } catch (java.io.IOException e) {
            return false;
        }
    }

    private static String env(String name, String fallback) {
        String v = System.getenv(name);
        return v == null || v.isEmpty() ? fallback : v;
    }
}
