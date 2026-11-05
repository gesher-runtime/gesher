/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.ContentInterface;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.IContentProvider;
import android.content.pm.ApplicationInfo;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.MediaStore;

import java.lang.reflect.Proxy;

/** Pure contracts and forwarding tested without a media provider or Binder server. */
public final class MediaStoreContractTest {
    private static int checks;
    private static Object[] call;
    private static String method;
    private static void check(boolean condition, String name) {
        if (!condition) throw new AssertionError(name);
        checks++;
    }
    private static void uri(Uri actual, String expected) { check(actual.toString().equals(expected), expected); }
    private static final class Resolver extends ContentResolver {
        Resolver(ContentInterface target) {
            super(new ContextWrapper(null) {
                @Override public String getOpPackageName() { return "gesher.test"; }
                @Override public ApplicationInfo getApplicationInfo() { return new ApplicationInfo(); }
            }, target);
        }
        @Override protected IContentProvider acquireProvider(Context c, String name) { throw new AssertionError("provider connection"); }
        @Override protected IContentProvider acquireUnstableProvider(Context c, String name) { throw new AssertionError("provider connection"); }
        @Override public boolean releaseProvider(IContentProvider p) { return true; }
        @Override public boolean releaseUnstableProvider(IContentProvider p) { return true; }
        @Override public void unstableProviderDied(IContentProvider p) {}
    }
    public static void main(String[] args) {
        System.loadLibrary("android_runtime");
        uri(MediaStore.Audio.Albums.getContentUri("external_primary"), "content://media/external_primary/audio/albums");
        uri(MediaStore.Audio.Artists.Albums.getContentUri("internal", 42), "content://media/internal/audio/artists/42/albums");
        uri(MediaStore.Audio.Genres.getContentUriForAudioId("external", 17), "content://media/external/audio/media/17/genres");
        uri(MediaStore.Audio.Genres.Members.getContentUri("external", 9), "content://media/external/audio/genres/9/members");
        uri(MediaStore.Audio.Playlists.Members.getContentUri("external_primary", 8), "content://media/external_primary/audio/playlists/8/members");
        uri(MediaStore.Video.Thumbnails.getContentUri("external"), "content://media/external/video/thumbnails");
        uri(MediaStore.Downloads.getContentUri("external_primary", 99), "content://media/external_primary/downloads/99");
        check(MediaStore.MediaColumns.GENERATION_ADDED.equals("generation_added")
                && MediaStore.MediaColumns.INFERRED_DATE.equals("inferred_date")
                && MediaStore.Audio.AudioColumns.BITS_PER_SAMPLE.equals("bits_per_sample"), "column contracts");
        check(MediaStore.PickerMediaColumns.DURATION_MILLIS.equals(MediaStore.MediaColumns.DURATION), "picker duration alias");
        check(MediaStore.MATCH_DEFAULT == 0 && MediaStore.MATCH_INCLUDE == 1
                && MediaStore.MATCH_EXCLUDE == 2 && MediaStore.MATCH_ONLY == 3, "query match constants");
        check(MediaStore.getPickImagesMaxLimit() == 100, "pinned picker maximum");
        Uri original = Uri.parse("content://media/external/images/media/5?existing=yes#fragment");
        Uri required = MediaStore.setRequireOriginal(original);
        check(!MediaStore.getRequireOriginal(original) && MediaStore.getRequireOriginal(required), "original query flag");
        check(required.getQueryParameter("existing").equals("yes") && required.getFragment().equals("fragment"), "URI query and fragment retained");
        check(MediaStore.setIncludePending(original).getBooleanQueryParameter("includePending", false), "pending query flag");
        check(MediaStore.Audio.keyFor(null).equals("") && MediaStore.Audio.keyFor("<unknown>").equals("01"), "legacy empty and unknown audio keys");
        check(MediaStore.Audio.keyFor("The Beatles").equals(MediaStore.Audio.keyFor("Beatles, The")), "audio article normalization");
        check(MediaStore.Audio.keyFor("\001Beatles").startsWith("01"), "audio sort-first marker");
        check(MediaStore.Images.Thumbnails.getKindSize(1).getWidth() == 512
                && MediaStore.Video.Thumbnails.getKindSize(3).getHeight() == 96, "thumbnail sizes");
        try { MediaStore.Video.Thumbnails.getKindSize(0); throw new AssertionError("invalid thumbnail kind"); }
        catch (IllegalArgumentException expected) { checks++; }

        ContentInterface target = (ContentInterface) Proxy.newProxyInstance(
                ContentInterface.class.getClassLoader(), new Class<?>[]{ContentInterface.class},
                (proxy, called, values) -> {
                    method = called.getName(); call = values;
                    if (method.equals("query")) return new MatrixCursor(new String[]{"_id"});
                    if (method.equals("update")) return 1;
                    throw new AssertionError("Unexpected resolver call: " + method);
                });
        Resolver resolver = new Resolver(target);
        MediaStore.Images.Media.query(resolver, original, new String[]{"_id"}, "width>?", new String[]{"20"}, null).close();
        Bundle query = (Bundle) call[2];
        check(method.equals("query") && call[0].equals(original)
                && query.getString(ContentResolver.QUERY_ARG_SQL_SELECTION).equals("width>?")
                && query.getStringArray(ContentResolver.QUERY_ARG_SQL_SELECTION_ARGS)[0].equals("20")
                && query.getString(ContentResolver.QUERY_ARG_SQL_SORT_ORDER).equals(MediaStore.Images.Media.DEFAULT_SORT_ORDER), "image query forwards selection, args and default sort");
        MediaStore.Video.query(resolver, original, new String[]{"_id"}).close();
        check(((Bundle)call[2]).getString(ContentResolver.QUERY_ARG_SQL_SORT_ORDER).equals(MediaStore.Video.DEFAULT_SORT_ORDER), "video default sort");
        MediaStore.Images.Thumbnails.queryMiniThumbnail(resolver, 7, 1, new String[]{"_id"}).close();
        check(((Bundle)call[2]).getString(ContentResolver.QUERY_ARG_SQL_SELECTION).equals("image_id = 7 AND kind = 1"), "thumbnail query selection");
        check(MediaStore.Audio.Playlists.Members.moveItem(resolver, 5, 2, 8), "playlist update result");
        uri((Uri)call[0], "content://media/external/audio/playlists/5/members/2?move=true");
        check(method.equals("update") && ((ContentValues)call[1]).getAsInteger("play_order") == 8, "playlist update order");
        System.out.println("MediaStoreContractTest all passed: " + checks);
    }
}
