/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.ActivityThread;
import android.app.ContentProviderHolder;
import android.app.GesherSettingsContext;
import android.app.IActivityManager;
import android.content.AttributionSource;
import android.content.Context;
import android.content.pm.IPackageManager;
import android.content.pm.SystemFeaturesCache;
import android.content.res.AssetManager;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Looper;
import android.os.Parcel;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.Process;
import android.provider.Settings;
import android.view.textservice.SentenceSuggestionsInfo;
import android.view.textservice.SpellCheckerSession;
import android.view.textservice.SpellCheckerSession.SpellCheckerSessionParams;
import android.view.textservice.SuggestionsInfo;
import android.view.textservice.TextServicesManager;

import com.android.internal.textservice.ISpellCheckerSession;
import com.android.internal.textservice.ISpellCheckerSessionListener;
import com.android.internal.textservice.ITextServicesManager;
import com.android.internal.textservice.ITextServicesSessionListener;
import com.gesher.server.Services;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.util.Collections;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;

/** Real public manager lookup and no-checker behavior through generated Binder dispatch. */
public final class TextServicesContractTest {
    private static int checks;
    private static void check(boolean condition, String reason) {
        if (!condition) throw new AssertionError(reason);
        checks++;
    }
    private interface Operation { void run() throws Exception; }
    private static void rejects(Class<? extends Throwable> expected, Operation action) throws Exception {
        try { action.run(); }
        catch (Throwable failure) {
            if (expected.isInstance(failure)) { checks++; return; }
            throw failure;
        }
        throw new AssertionError("Expected " + expected.getSimpleName());
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        if (Looper.myLooper() == null) Looper.prepareMainLooper();
        GesherSettingsContext.prepare();
        Map<String, IBinder> created = Services.createInProcess(Collections.emptyList());
        IBinder target = created.get(Context.TEXT_SERVICES_MANAGER_SERVICE);
        check(target != null, "textservices is registered");
        AtomicInteger transactions = new AtomicInteger();
        IBinder transport = new Binder() {
            @Override public IInterface queryLocalInterface(String descriptor) { return null; }
            @Override protected boolean onTransact(int code, Parcel request, Parcel reply, int flags)
                    throws RemoteException {
                transactions.incrementAndGet();
                return target.transact(code, request, reply, flags);
            }
        };
        created.put(Context.TEXT_SERVICES_MANAGER_SERVICE, transport);
        Field cacheField = ServiceManager.class.getDeclaredField("sCache");
        cacheField.setAccessible(true);
        @SuppressWarnings("unchecked") Map<String, IBinder> cache =
                (Map<String, IBinder>) cacheField.get(null);
        cache.putAll(created);
        // ActivityThread.bindApplication normally supplies this cache before
        // SystemServiceRegistry checks device features during manager lookup.
        SystemFeaturesCache.setInstance(new SystemFeaturesCache(IPackageManager.Stub
                .asInterface(created.get("package")).getSystemAvailableFeatures().getList()));
        Constructor<ActivityThread> constructor = ActivityThread.class.getDeclaredConstructor();
        constructor.setAccessible(true);
        Context context = constructor.newInstance().getSystemContext();
        // Like normal startup, acquire the provider only after its framework
        // context can resolve the registered display and package services.
        ContentProviderHolder settingsHolder = IActivityManager.Stub.asInterface(created.get(Context.ACTIVITY_SERVICE))
                .getContentProviderExternal(Settings.AUTHORITY, 0, null, "text-services-contract");
        Bundle disabled = new Bundle();
        disabled.putString("value", "0");
        rejects(SecurityException.class, () -> settingsHolder.provider.call(
                new AttributionSource(Process.myUid(), "android", null), Settings.AUTHORITY,
                "PUT_secure", Settings.Secure.SPELL_CHECKER_ENABLED, disabled));
        rejects(SecurityException.class, () -> settingsHolder.provider.call(
                new AttributionSource(Process.myUid(), "android", null), Settings.AUTHORITY,
                "gesher:select-default-spell-checker", null, null));
        TextServicesManager manager = context.getSystemService(TextServicesManager.class);
        check(manager != null && manager.getClass() == TextServicesManager.class,
                "real public TextServicesManager constructs");
        check(manager == context.getSystemService(Context.TEXT_SERVICES_MANAGER_SERVICE),
                "typed and named lookups share manager");
        check(manager.isSpellCheckerEnabled(), "default enabled preference is independent of provider availability");
        check(manager.getCurrentSpellCheckerInfo() == null
                        && manager.getCurrentSpellCheckerSubtype(true) == null,
                "no current checker or subtype is invented");
        check(manager.getEnabledSpellCheckers() == null, "legacy raw empty-array shape matches AOSP");
        check(manager.getEnabledSpellCheckerInfos().isEmpty(), "public list normalizes absence");
        AtomicInteger callbacks = new AtomicInteger();
        SpellCheckerSession.SpellCheckerSessionListener listener =
                new SpellCheckerSession.SpellCheckerSessionListener() {
                    @Override public void onGetSuggestions(SuggestionsInfo[] values) {
                        callbacks.incrementAndGet();
                    }
                    @Override public void onGetSentenceSuggestions(SentenceSuggestionsInfo[] values) {
                        callbacks.incrementAndGet();
                    }
                };
        check(manager.newSpellCheckerSession(null, null, listener, true) == null,
                "old settings-based session reports unavailable");
        check(manager.newSpellCheckerSession(null, Locale.US, listener, false) == null,
                "old explicit-locale session reports unavailable");
        SpellCheckerSessionParams settings = new SpellCheckerSessionParams.Builder()
                .setShouldReferToSpellCheckerLanguageSettings(true).build();
        SpellCheckerSessionParams explicit = new SpellCheckerSessionParams.Builder()
                .setLocale(Locale.US).build();
        check(manager.newSpellCheckerSession(settings, Runnable::run, listener) == null
                        && manager.newSpellCheckerSession(explicit, Runnable::run, listener) == null,
                "new session overloads report unavailable");
        rejects(IllegalArgumentException.class,
                () -> manager.newSpellCheckerSession(null, null, listener, false));
        rejects(NullPointerException.class,
                () -> manager.newSpellCheckerSession(settings, null, listener));
        rejects(NullPointerException.class,
                () -> manager.newSpellCheckerSession(settings, Runnable::run, null));

        ITextServicesManager service = ITextServicesManager.Stub.asInterface(transport);
        check(service.asBinder() == transport, "forced Binder proxy is in use");
        rejects(SecurityException.class, () -> service.isSpellCheckerEnabled(10));
        rejects(SecurityException.class, () -> service.getEnabledSpellCheckers(-1));
        ISpellCheckerSessionListener rawListener = new ISpellCheckerSessionListener.Stub() {
            @Override public void onGetSuggestions(SuggestionsInfo[] values) { callbacks.incrementAndGet(); }
            @Override public void onGetSentenceSuggestions(SentenceSuggestionsInfo[] values) {
                callbacks.incrementAndGet();
            }
        };
        service.getSpellCheckerService(0, "missing.checker", "en_US",
                new ITextServicesSessionListener.Stub() {
                    @Override public void onServiceConnected(ISpellCheckerSession session) {
                        callbacks.incrementAndGet();
                    }
                }, rawListener, null, 0);
        service.finishSpellCheckerService(0, rawListener);
        service.finishSpellCheckerService(0, rawListener);
        check(callbacks.get() == 0, "unknown checker creates no fabricated callback or session");
        check(transactions.get() >= 12, "public and raw calls traversed generated Binder dispatch");
        check(TextServicesManager.getInstance() != null, "legacy static manager also constructs");
        System.out.println("TextServicesContractTest all passed: " + checks);
        System.exit(0);
    }
}
