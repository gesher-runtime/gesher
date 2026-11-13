/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.PendingIntent;
import android.content.ContentResolver;
import android.net.ConnectivityManager;
import android.net.NetworkCapabilities;
import android.net.Uri;
import android.provider.MediaStore;

import java.util.Collection;

/** Compiled separately against SDK 36 by the host runner, without production classes. */
public final class MediaStoreRequestSdkCalls {
    public static PendingIntent create(ContentResolver resolver, Collection<Uri> uris,
            String operation, boolean value) {
        switch (operation) {
            case "write": return MediaStore.createWriteRequest(resolver, uris);
            case "delete": return MediaStore.createDeleteRequest(resolver, uris);
            case "trash": return MediaStore.createTrashRequest(resolver, uris, value);
            case "favorite": return MediaStore.createFavoriteRequest(resolver, uris, value);
            default: throw new AssertionError(operation);
        }
    }

    public static void mark(ContentResolver resolver, Collection<Uri> uris, boolean value) {
        MediaStore.markIsFavoriteStatus(resolver, uris, value);
    }

    public static boolean gallery(ContentResolver resolver, int uid, String packageName) {
        return MediaStore.isCurrentSystemGallery(resolver, uid, packageName);
    }

    public static boolean cloud(ContentResolver resolver, String authority, boolean current) {
        return current ? MediaStore.isCurrentCloudMediaProviderAuthority(resolver, authority)
                : MediaStore.isSupportedCloudMediaProviderAuthority(resolver, authority);
    }

    public static void notifyCloud(ContentResolver resolver, String authority, String collection) {
        MediaStore.notifyCloudMediaChangedEvent(resolver, authority, collection);
    }

    @SuppressWarnings("deprecation")
    public static void preference(ConnectivityManager manager, int preference) {
        manager.setNetworkPreference(preference);
    }

    public static void reserved(ConnectivityManager.NetworkCallback callback,
            NetworkCapabilities capabilities) {
        callback.onReserved(capabilities);
    }
}
