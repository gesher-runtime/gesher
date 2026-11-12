/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.PendingIntent;
import android.content.AttributionSource;
import android.content.ClipData;
import android.content.ContentInterface;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.IContentProvider;
import android.content.IIntentSender;
import android.content.pm.ApplicationInfo;
import android.net.ConnectivityManager;
import android.net.NetworkCapabilities;
import android.net.Uri;
import android.os.Bundle;
import android.os.RemoteException;

import java.lang.reflect.Proxy;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.NoSuchElementException;

/** Real framework resolver/values with a controlled provider boundary; no media database or UI. */
public final class MediaStoreRequestContractTest {
    private static int checks;

    private interface Operation { void run(); }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }

    private static void expect(Class<? extends Throwable> type, Operation operation,
            String message) {
        try {
            operation.run();
            throw new AssertionError(message + ": no exception");
        } catch (Throwable error) {
            if (!type.isInstance(error)) throw new AssertionError(message, error);
            checks++;
        }
    }

    private static final class Provider {
        final PendingIntent approval = new PendingIntent(new IIntentSender.Default());
        int calls;
        String authority;
        String method;
        String arg;
        Bundle extras;
        Bundle reply;
        Throwable failure;
        final IContentProvider transport = (IContentProvider) Proxy.newProxyInstance(
                IContentProvider.class.getClassLoader(), new Class<?>[]{IContentProvider.class},
                (proxy, called, args) -> {
                    if (!called.getName().equals("call") || args.length != 5) {
                        throw new AssertionError("Unexpected provider operation: " + called);
                    }
                    return call((String) args[1], (String) args[2], (String) args[3],
                            (Bundle) args[4]);
                });

        Bundle call(String authority, String method, String arg, Bundle extras) throws Throwable {
            calls++;
            this.authority = authority;
            this.method = method;
            this.arg = arg;
            this.extras = extras;
            if (failure != null) throw failure;
            return reply;
        }

        void approved() {
            reply = new Bundle();
            reply.putParcelable("result", approval);
        }

        void result(String key, boolean value) {
            reply = new Bundle();
            reply.putBoolean(key, value);
        }

        void route(String method, String arg) {
            check("media".equals(authority) && method.equals(this.method)
                    && java.util.Objects.equals(arg, this.arg), "provider route: " + method);
        }

        void items(List<Uri> uris) {
            ClipData clip = extras.getParcelable("clip_data", ClipData.class);
            check(clip.getItemCount() == uris.size(), "all requested items retained");
            for (int i = 0; i < uris.size(); i++) {
                check(uris.get(i).equals(clip.getItemAt(i).getUri()), "requested item order " + i);
            }
        }
    }

    private static final class Resolver extends ContentResolver {
        final Provider provider;
        boolean available = true;
        int acquired;
        int released;

        Resolver(Provider provider) { this(provider, null); }

        Resolver(Provider provider, ContentInterface wrapped) {
            super(new ContextWrapper(null) {
                @Override public String getOpPackageName() { return "gesher.test"; }
                @Override public ApplicationInfo getApplicationInfo() {
                    ApplicationInfo info = new ApplicationInfo();
                    info.targetSdkVersion = 36;
                    return info;
                }
                // Identity/authorization belongs to the provider, outside this forwarding fixture.
                @Override public AttributionSource getAttributionSource() { return null; }
            }, wrapped);
            this.provider = provider;
        }

        @Override protected IContentProvider acquireProvider(Context context, String name) {
            check("media".equals(name), "resolver acquires the media authority");
            acquired++;
            return available ? provider.transport : null;
        }

        @Override public boolean releaseProvider(IContentProvider provider) {
            check(provider == this.provider.transport, "resolver releases the acquired provider");
            released++;
            return true;
        }

        @Override protected IContentProvider acquireUnstableProvider(Context context, String name) {
            throw new AssertionError("call must acquire a stable provider");
        }

        @Override public boolean releaseUnstableProvider(IContentProvider provider) {
            throw new AssertionError("unexpected unstable provider release");
        }

        @Override public void unstableProviderDied(IContentProvider provider) {
            throw new AssertionError("unexpected unstable provider death");
        }
    }

    public static void contracts() {
        Provider provider = new Provider();
        Resolver resolver = new Resolver(provider);
        List<Uri> uris = Arrays.asList(Uri.parse("content://media/external/images/media/7"),
                Uri.parse("content://media/external_primary/video/media/12?original=1"));
        provider.approved();
        for (String operation : new String[]{"write", "delete", "trash", "favorite"}) {
            for (boolean value : new boolean[]{false, true}) {
                check(MediaStoreRequestSdkCalls.create(resolver, uris, operation, value)
                        == provider.approval, "returns provider's approval object: " + operation);
                provider.route("create_" + operation + "_request", null);
                provider.items(uris);
                check(provider.extras.containsKey("content_values"), "values envelope is present");
                ContentValues values = provider.extras.getParcelable("content_values", ContentValues.class);
                if (operation.equals("trash") || operation.equals("favorite")) {
                    check(values.size() == 1 && values.get("is_" + (operation.equals("trash")
                            ? "trashed" : "favorite")) instanceof Integer
                            && values.getAsInteger("is_" + (operation.equals("trash")
                            ? "trashed" : "favorite")) == (value ? 1 : 0), "numeric change value");
                } else {
                    check(values == null, "write/delete do not mutate metadata");
                }
            }
        }

        for (boolean value : new boolean[]{false, true}) {
            MediaStoreRequestSdkCalls.mark(resolver, uris, value);
            provider.route("mark_media_as_favorite", null);
            provider.items(uris);
            check(provider.extras.size() == 2 && provider.extras.get("is_favorite") instanceof Boolean
                    && provider.extras.getBoolean("is_favorite") == value, "mark uses boolean status");
        }

        int before = provider.calls;
        for (String operation : new String[]{"write", "delete", "trash", "favorite"}) {
            expect(NullPointerException.class,
                    () -> MediaStoreRequestSdkCalls.create(null, uris, operation, false), "null resolver");
            expect(NullPointerException.class,
                    () -> MediaStoreRequestSdkCalls.create(resolver, null, operation, false), "null items");
            expect(NoSuchElementException.class,
                    () -> MediaStoreRequestSdkCalls.create(resolver, Collections.emptyList(), operation,
                            false), "empty items retain pinned contract");
        }
        expect(NullPointerException.class, () -> MediaStoreRequestSdkCalls.mark(null, uris, true),
                "mark null resolver");
        expect(NullPointerException.class, () -> MediaStoreRequestSdkCalls.mark(resolver, null, true),
                "mark null items");
        expect(NoSuchElementException.class,
                () -> MediaStoreRequestSdkCalls.mark(resolver, Collections.emptyList(), true),
                "mark empty items");
        check(provider.calls == before, "invalid request envelopes never reach provider");

        for (boolean allowed : new boolean[]{false, true}) {
            provider.result("is_system_gallery_response", allowed);
            check(MediaStoreRequestSdkCalls.gallery(resolver, 10042, "example.gallery") == allowed,
                    "gallery result comes from provider");
            provider.route("is_system_gallery", "example.gallery");
            check(provider.extras.size() == 1
                    && provider.extras.getInt("is_system_gallery_uid") == 10042, "gallery UID forwarded");
            for (boolean current : new boolean[]{false, true}) {
                provider.result("cloud_provider_result", allowed);
                check(MediaStoreRequestSdkCalls.cloud(resolver, "example.cloud", current) == allowed,
                        "cloud status comes from provider");
                provider.route(current ? "is_current_cloud_provider" : "is_supported_cloud_provider",
                        "example.cloud");
                check(provider.extras == null, "cloud query has no extra payload");
            }
        }
        for (boolean current : new boolean[]{false, true}) {
            expect(NullPointerException.class,
                    () -> MediaStoreRequestSdkCalls.cloud(null, "example.cloud", current),
                    "cloud null resolver");
            expect(NullPointerException.class,
                    () -> MediaStoreRequestSdkCalls.cloud(resolver, null, current),
                    "cloud null authority");
        }

        provider.result("cloud_provider_result", false);
        expect(SecurityException.class,
                () -> MediaStoreRequestSdkCalls.notifyCloud(resolver, "example.cloud", "collection-3"),
                "denied cloud event is not reported as completed");
        provider.route("notify_cloud_media_changed_event", "example.cloud");
        check(provider.extras.size() == 1 && "collection-3".equals(
                provider.extras.getString("android.provider.extra.MEDIA_COLLECTION_ID")),
                "cloud event collection forwarded");
        provider.result("cloud_provider_result", true);
        MediaStoreRequestSdkCalls.notifyCloud(resolver, "example.cloud", "collection-4");
        check("collection-4".equals(provider.extras.getString("android.provider.extra.MEDIA_COLLECTION_ID")),
                "accepted cloud event retains current collection");
        expect(NullPointerException.class,
                () -> MediaStoreRequestSdkCalls.notifyCloud(null, "example.cloud", "collection"),
                "cloud event null resolver");
        expect(NullPointerException.class,
                () -> MediaStoreRequestSdkCalls.notifyCloud(resolver, null, "collection"),
                "cloud event null authority");

        Operation[] requests = {
                () -> MediaStoreRequestSdkCalls.create(resolver, uris, "write", false),
                () -> MediaStoreRequestSdkCalls.create(resolver, uris, "delete", false),
                () -> MediaStoreRequestSdkCalls.create(resolver, uris, "trash", false),
                () -> MediaStoreRequestSdkCalls.create(resolver, uris, "favorite", false),
                () -> MediaStoreRequestSdkCalls.mark(resolver, uris, false),
                () -> MediaStoreRequestSdkCalls.gallery(resolver, 10042, "example.gallery"),
                () -> MediaStoreRequestSdkCalls.cloud(resolver, "example.cloud", false),
                () -> MediaStoreRequestSdkCalls.cloud(resolver, "example.cloud", true),
                () -> MediaStoreRequestSdkCalls.notifyCloud(resolver, "example.cloud", "collection")
        };
        SecurityException denial = new SecurityException("controlled provider permission denial");
        provider.failure = denial;
        for (Operation request : requests) {
            try { request.run(); throw new AssertionError("provider denial swallowed"); }
            catch (SecurityException expected) { check(expected == denial, "original provider denial retained"); }
        }
        provider.failure = null;
        check(resolver.acquired == resolver.released, "provider references released after success and errors");
        resolver.available = false;
        before = provider.calls;
        for (Operation request : requests) {
            expect(IllegalArgumentException.class, request, "absent provider retains resolver failure");
        }
        check(provider.calls == before, "absent provider never processes a request");
        resolver.available = true;

        provider.reply = null;
        expect(NullPointerException.class,
                () -> MediaStoreRequestSdkCalls.create(resolver, uris, "write", false),
                "null response cannot synthesize an approval");
        provider.reply = new Bundle();
        check(MediaStoreRequestSdkCalls.create(resolver, uris, "write", false) == null,
                "empty response retains pinned missing-result behavior");
        check(!MediaStoreRequestSdkCalls.cloud(resolver, "example.cloud", true),
                "missing cloud result cannot report support");

        ContentInterface wrapped = (ContentInterface) Proxy.newProxyInstance(
                ContentInterface.class.getClassLoader(), new Class<?>[]{ContentInterface.class},
                (proxy, method, args) -> {
                    if (!method.getName().equals("call")) throw new AssertionError(method);
                    return provider.call((String) args[0], (String) args[1], (String) args[2],
                            (Bundle) args[3]);
                });
        Resolver wrappedResolver = new Resolver(provider, wrapped);
        provider.approved();
        check(MediaStoreRequestSdkCalls.create(wrappedResolver, uris, "write", false) == provider.approval,
                "wrapped ContentInterface receives request and supplies result");
        check(wrappedResolver.acquired == 0, "wrapped resolver avoids unrelated provider acquisition");
        provider.failure = new RemoteException("controlled provider death");
        expect(NullPointerException.class,
                () -> MediaStoreRequestSdkCalls.create(wrappedResolver, uris, "write", false),
                "wrapped remote failure cannot synthesize an approval");
        provider.failure = null;

        ConnectivityManager manager = new ConnectivityManager(new ContextWrapper(null));
        MediaStoreRequestSdkCalls.preference(manager, ConnectivityManager.TYPE_WIFI);
        MediaStoreRequestSdkCalls.preference(manager, Integer.MIN_VALUE);
        check(manager.getNetworkPreference() == ConnectivityManager.TYPE_NONE,
                "removed network preference retains no-effect contract");
        NetworkCapabilities capabilities = new NetworkCapabilities();
        MediaStoreRequestSdkCalls.reserved(new ConnectivityManager.NetworkCallback(), capabilities);
        final int[] callbacks = {0};
        MediaStoreRequestSdkCalls.reserved(new ConnectivityManager.NetworkCallback() {
            @Override public void onReserved(NetworkCapabilities received) {
                super.onReserved(received);
                check(received == capabilities, "reserved callback receives original capabilities");
                callbacks[0]++;
            }
        }, capabilities);
        check(callbacks[0] == 1, "reserved base hook remains overridable");
    }

    public static void main(String[] args) {
        if (args.length == 0) System.loadLibrary("android_runtime");
        else System.load(args[0]);
        contracts();
        System.out.println("MediaStoreRequestContractTest all passed: " + checks);
    }
}
