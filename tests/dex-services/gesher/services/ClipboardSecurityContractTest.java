/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.app.AppOpsManager;
import android.app.ContentProviderHolder;
import android.content.AttributionSource;
import android.content.ClipData;
import android.content.ContentProvider;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.os.Binder;
import com.gesher.server.ApkPackage;
import com.gesher.server.UriGrants;
import java.io.File;
import java.lang.reflect.InvocationTargetException;
import java.util.LinkedHashMap;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import static gesher.services.ClipboardFixture.*;
import static android.app.AppOpsManager.*;

/** Android 16 clipboard contracts against production authentication and permission/AppOps policy. */
public final class ClipboardSecurityContractTest {
    private static int checks;
    private static void check(boolean value, String why) { if (!value) throw new AssertionError(why); checks++; }
    private static void rejects(Class<? extends Throwable> expected, Checked action) throws Exception {
        try { action.run(); }
        catch (Throwable failure) {
            if (failure instanceof InvocationTargetException wrapped) failure = wrapped.getCause();
            if (expected.isInstance(failure)) { checks++; return; }
            throw new AssertionError("Expected " + expected + ", got " + failure, failure);
        }
        throw new AssertionError("Expected " + expected.getSimpleName());
    }
    private static void read(ClipboardFixture h, ApkPackage code, boolean allowed, String why) throws Exception {
        h.as(code, () -> {
            check((h.api.getPrimaryClip(code.packageName, "clipboard", 0, 0) != null) == allowed, why + ": get");
            check((h.api.getPrimaryClipDescription(code.packageName, "clipboard", 0, 0) != null) == allowed, why + ": description");
            check(h.api.hasPrimaryClip(code.packageName, "clipboard", 0, 0) == allowed, why + ": has");
            check(h.api.hasClipboardText(code.packageName, "clipboard", 0, 0) == allowed, why + ": text");
        });
    }
    private static long noted(ClipboardFixture h, ApkPackage code) {
        List<AppOpsManager.PackageOps> packages = h.ops.getOpsForPackage(code.uid(), code.packageName, new int[]{OP_READ_CLIPBOARD});
        long result = -1;
        if (packages != null) for (var pkg : packages) for (var op : pkg.getOps()) result = Math.max(result, op.getLastAccessTime(OP_FLAGS_ALL));
        return result;
    }
    private static void focusAndIdentity(ClipboardFixture h) throws Exception {
        check(h.pm.permissions().checkPermission(OWNER, Manifest.permission.READ_CLIPBOARD_IN_BACKGROUND)
                == PackageManager.PERMISSION_DENIED, "Requested signature permission is not a grant");
        h.write("initial"); read(h, h.owner, true, "Actual foreground generation");
        read(h, h.peer, false, "Other registered application");
        h.resumed(true);
        h.nativeState.foreground = null; read(h, h.owner, false, "A still-resumed Android task has lost Windows activation");
        h.resumed(false);
        h.nativeState.foreground = new long[]{PID, 0}; read(h, h.owner, false, "Unknown native generation");
        h.nativeState.foreground = new long[]{PID, CREATED + 1}; read(h, h.owner, false, "PID generation changed");
        h.nativeState.foreground = new long[]{PID + 77, CREATED}; read(h, h.owner, false, "Unregistered Windows process");
        h.nativeState.foreground = new long[]{PID + 1, CREATED + 1}; read(h, h.peer, true, "Focus follows the other Windows process");
        h.nativeState.foreground = new long[]{PID, CREATED};
        Object process = h.processes.get(0);
        set(process, "exiting", true); read(h, h.owner, false, "Exiting process"); set(process, "exiting", false);
        for (String state : List.of("dead", "pidSuperseded")) {
            set(process, state, true);
            check(!(Boolean)call(h.atm, "ownsForegroundProcess", new Class<?>[]{int.class, long[].class}, UID,
                    new long[]{PID, CREATED}), "Retired process cannot own focus: " + state);
            h.asOwner(() -> rejects(SecurityException.class, () -> h.api.getPrimaryClip(OWNER, null, 0, 0)));
            set(process, state, false);
        }
        set(process, "ready", false); read(h, h.owner, false, "Unfinished attachment"); set(process, "ready", true);
        set(process, "nativeStartTime", 0L); read(h, h.owner, false, "Unknown attachment generation"); set(process, "nativeStartTime", CREATED);
        set(process, "packageCode", pkg(OWNER, UID, (byte)7));
        read(h, h.owner, false, "A retired package-code generation cannot own focus"); set(process, "packageCode", h.owner);
        h.asOwner(() -> {
            rejects(SecurityException.class, () -> h.api.getPrimaryClip(PEER, null, 0, 0));
            rejects(SecurityException.class, () -> h.api.hasPrimaryClip(null, null, 0, 0));
            rejects(SecurityException.class, () -> h.api.setPrimaryClip(ClipData.newPlainText("x", "x"), "unknown.package", null, 0, 0));
            rejects(SecurityException.class, () -> h.api.getPrimaryClipSource(OWNER, null, 0, 0));
            rejects(SecurityException.class, () -> h.api.setPrimaryClipAsPackage(ClipData.newPlainText("x", "x"), OWNER, null, 0, 0, PEER));
            rejects(IllegalArgumentException.class, () -> h.api.setPrimaryClip(null, OWNER, null, 0, 0));
            check(h.api.getPrimaryClip(OWNER, null, 91, 52) != null,
                    "Single-user/no-VDM AOSP mapping uses the caller/default clipboard");
        });
        long identity = Binder.clearCallingIdentity();
        try {
            Binder.restoreCallingIdentity(((long)UID << 32) | (PID + 99));
            rejects(SecurityException.class, () -> h.api.getPrimaryClip(OWNER, null, 0, 0));
        } finally { Binder.restoreCallingIdentity(identity); }
    }
    private static void imeAndPermissions(ClipboardFixture h) throws Exception {
        h.nativeState.foreground = null;
        h.asOwner(() -> rejects(SecurityException.class, () -> h.ime(OWNER + "/.Keyboard")));
        read(h, h.owner, false, "Unprivileged app cannot assign itself as the secure default IME");
        read(h, h.ime, false, "No configured IME");
        read(h, h.hostPackage, false, "Built-in package name alone gives no IME authority");
        h.configureIme(IME + "/.Keyboard"); read(h, h.ime, true, "Actual secure default IME setting");
        h.configureIme("malformed"); read(h, h.ime, false, "Malformed configured component");
        h.configureIme(PEER + "/.Keyboard"); read(h, h.ime, false, "IME configuration changed");
        read(h, h.peer, true, "New configured IME"); h.configureIme(null);
        check(h.pm.permissions().checkPermission(PRIVILEGED, Manifest.permission.READ_CLIPBOARD_IN_BACKGROUND)
                == PackageManager.PERMISSION_GRANTED, "Platform signature grants the requested Android permission");
        read(h, h.privileged, true, "Granted background clipboard permission");
        h.nativeState.lockState = 1;
        read(h, h.privileged, false, "Lock also gates privileged getters");
        h.as(h.privileged, () -> check(h.api.getPrimaryClipSource(PRIVILEGED, null, 0, 0) == null, "Locked source is unavailable"));
        h.configureIme(IME + "/.Keyboard"); read(h, h.ime, false, "Lock also gates the default IME");
        h.nativeState.lockState = -1; read(h, h.privileged, false, "Unknown lock state fails closed");
        h.nativeState.lockState = 0; h.configureIme(null);
    }
    private static void appOps(ClipboardFixture h) throws Exception {
        h.nativeState.foreground = new long[]{PID, CREATED};
        for (int mode : new int[]{MODE_IGNORED, MODE_ERRORED, MODE_DEFAULT}) {
            h.ops.setMode(OP_READ_CLIPBOARD, UID, OWNER, mode);
            read(h, h.owner, false, "Non-allowed read AppOps mode " + mode);
        }
        h.ops.setMode(OP_READ_CLIPBOARD, UID, OWNER, MODE_ALLOWED);
        h.ops.setUidMode(OP_READ_CLIPBOARD, UID, MODE_IGNORED);
        read(h, h.owner, false, "UID policy takes precedence");
        h.ops.setUidMode(OP_READ_CLIPBOARD, UID, MODE_ALLOWED);
        h.ops.setMode(OP_READ_CLIPBOARD, h.privileged.uid(), PRIVILEGED, MODE_IGNORED);
        read(h, h.privileged, false, "Permission does not bypass AppOps");
        h.ops.setMode(OP_READ_CLIPBOARD, h.privileged.uid(), PRIVILEGED, MODE_ALLOWED);
        h.configureIme(IME + "/.Keyboard"); h.ops.setMode(OP_READ_CLIPBOARD, h.ime.uid(), IME, MODE_IGNORED);
        read(h, h.ime, false, "IME does not bypass AppOps");
        h.ops.setMode(OP_READ_CLIPBOARD, h.ime.uid(), IME, MODE_ALLOWED); h.configureIme(null);
        int writes = h.nativeState.writes, clears = h.nativeState.clears;
        h.ops.setMode(OP_WRITE_CLIPBOARD, UID, OWNER, MODE_IGNORED);
        h.write("denied write");
        h.asOwner(() -> h.api.clearPrimaryClip(OWNER, null, 0, 0));
        check(h.nativeState.writes == writes && h.nativeState.clears == clears, "Denied writes/clear never reach Windows");
        h.asOwner(() -> check("initial".contentEquals(h.api.getPrimaryClip(OWNER, null, 0, 0).getItemAt(0).getText()), "Denied write preserves data"));
        h.ops.setMode(OP_WRITE_CLIPBOARD, UID, OWNER, MODE_ALLOWED);
        h.nativeState.foreground = null; h.write("background write");
        check("background write".equals(h.nativeState.text), "Unfocused writes remain allowed");
        long before = noted(h, h.peer);
        h.configureIme(PEER + "/.Keyboard");
        h.as(h.peer, () -> {
            h.api.hasPrimaryClip(PEER, "clipboard", 0, 0);
            h.api.hasClipboardText(PEER, "clipboard", 0, 0);
            h.api.getPrimaryClipDescription(PEER, "clipboard", 0, 0);
        });
        check(noted(h, h.peer) == before, "Metadata checks do not note clipboard reads");
        h.as(h.peer, () -> h.api.getPrimaryClip(PEER, "clipboard", 0, 0));
        var attributed = h.ops.getOpsForPackage(h.peer.uid(), PEER, new int[]{OP_READ_CLIPBOARD}).get(0).getOps().get(0).getAttributedOpEntries();
        check(attributed.containsKey("clipboard") && noted(h, h.peer) >= before, "A data read notes the declared attribution"); h.configureIme(null);
    }
    private static void sourceAndStorage(ClipboardFixture h) throws Exception {
        h.as(h.privileged, () -> {
            check(OWNER.equals(h.api.getPrimaryClipSource(PRIVILEGED, null, 0, 0)), "Write source remains the authenticated package");
            h.api.setPrimaryClipAsPackage(ClipData.newPlainText("source", "chosen source"), PRIVILEGED, null, 0, 0, PEER);
            check(PEER.equals(h.api.getPrimaryClipSource(PRIVILEGED, null, 0, 0)), "Authorized setAsPackage keeps its source separate from caller");
        });
        h.nativeState.changeAfterWrite = true; h.write("own write"); h.nativeState.changeAfterWrite = false;
        h.as(h.privileged, () -> {
            check("external-after-write".contentEquals(h.api.getPrimaryClip(PRIVILEGED, null, 0, 0).getItemAt(0).getText()),
                    "An external write after native publication is observed");
            check(h.api.getPrimaryClipSource(PRIVILEGED, null, 0, 0) == null,
                    "An external sequence never inherits the preceding Android writer's provenance");
        });
        h.write("last real native data");
        h.nativeState.rejectWrite = true;
        try {
            h.asOwner(() -> h.api.setPrimaryClip(ClipData.newRawUri("unpublished private URI",
                    Uri.parse("content://fixture.provider/not-published")), OWNER, null, 0, 0));
        } finally { h.nativeState.rejectWrite = false; }
        h.as(h.privileged, () -> {
            ClipData actual = h.api.getPrimaryClip(PRIVILEGED, null, 0, 0);
            check(actual != null && actual.getItemAt(0).getText() != null
                    && "last real native data".contentEquals(actual.getItemAt(0).getText())
                    && actual.getItemAt(0).getUri() == null,
                    "A failed unbound write cannot retain private URI data over an unchanged old native generation");
            check(h.api.getPrimaryClipSource(PRIVILEGED, null, 0, 0) == null,
                    "An unavailable receipt never invents Android source ownership");
        });
        Uri privateUri = Uri.parse("content://fixture.provider/same-looking-projection");
        h.asOwner(() -> h.api.setPrimaryClip(ClipData.newRawUri("private", privateUri), OWNER, null, 0, 0));
        h.as(h.privileged, () -> check(privateUri.equals(h.api.getPrimaryClip(PRIVILEGED, null, 0, 0).getItemAt(0).getUri()),
                "A completed own publication retains its original typed URI"));
        h.nativeState.external(privateUri.toString(), null);
        h.as(h.privileged, () -> {
            ClipData actual = h.api.getPrimaryClip(PRIVILEGED, null, 0, 0);
            check(actual != null && actual.getItemAt(0).getText() != null
                    && privateUri.toString().contentEquals(actual.getItemAt(0).getText())
                    && actual.getItemAt(0).getUri() == null,
                    "A later same-looking native generation cannot revive stale typed URI data");
            check(h.api.getPrimaryClipSource(PRIVILEGED, null, 0, 0) == null,
                    "Equal native text never re-adopts the earlier Android source");
        });
        h.nativeState.external("external", null);
        h.as(h.privileged, () -> check(h.api.getPrimaryClipSource(PRIVILEGED, null, 0, 0) == null, "Windows content retires Android source attribution"));
        File image = File.createTempFile("clipboard-fixture-", ".png");
        try {
            java.nio.file.Files.write(image.toPath(), new byte[]{1, 2, 3});
            h.asOwner(() -> h.api.setPrimaryClip(ClipData.newRawUri("provider",
                    Uri.parse("content://fixture.provider" + image.getAbsolutePath())), OWNER, null, 0, 0));
            check(h.nativeState.image == null, "Content-provider URI paths are not read as local files");
        } finally { image.delete(); }
        Listener listener = new Listener(); h.add(h.privileged, listener);
        h.asOwner(() -> h.api.clearPrimaryClip(OWNER, null, 0, 0));
        int calls = listener.calls.get();
        check(calls == 1 && h.nativeState.text == null && h.nativeState.image == null, "Clear empties native formats and announces the transition");
        h.asOwner(() -> h.api.clearPrimaryClip(OWNER, null, 0, 0));
        check(listener.calls.get() == calls, "Empty-to-empty clear follows AOSP's no-notification behavior");
        h.as(h.privileged, () -> check(h.api.getPrimaryClipSource(PRIVILEGED, null, 0, 0) == null, "Clear retires source attribution"));
        h.remove(h.privileged, listener);
    }
    private static void nativeImageGrants(ClipboardFixture h, boolean binder) throws Exception {
        byte[] image = {'B', 'M', 0, 1};
        h.nativeState.external("native text", image);
        h.as(h.privileged, () -> check(h.api.hasPrimaryClip(PRIVILEGED, null, 0, 0),
                "Image metadata does not consume the clipboard"));
        Uri uri = (Uri)get(h.service, "mOwnedImage");
        check(uri != null && "content".equals(uri.getScheme()),
                "Native image is a content capability with no exposed filesystem path");
        UriGrants grants = (UriGrants) call(h.atm, "uriGrants", new Class<?>[0]);
        int read = Intent.FLAG_GRANT_READ_URI_PERMISSION;
        int write = Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
        check(!grants.canAccess(h.privileged, uri, read, 0), "Metadata queries cannot issue image read grants");
        Object provider = call(h.atm, "clipboardImages", new Class<?>[0]);
        call(provider, "attachTo", new Class<?>[]{Context.class}, new ContextWrapper(null) {
            @Override public int getUserId() { return 0; }
            @Override public Object getSystemService(String name) { return null; }
        });
        var transport = ((ContentProvider)provider).getIContentProvider();
        var granted = new AttributionSource(h.privileged.uid(), PRIVILEGED, null);
        var denied = new AttributionSource(h.peer.uid(), PEER, null);
        h.as(h.privileged, () -> rejects(SecurityException.class,
                () -> transport.query(granted, uri, null, new Bundle(), null)));
        h.as(h.privileged, () -> {
            ClipData clip = h.api.getPrimaryClip(PRIVILEGED, null, 0, 0);
            check(clip != null && uri.equals(clip.getItemAt(0).getUri())
                    && "native text".contentEquals(clip.getItemAt(0).getText())
                    && clip.getDescription().hasMimeType("image/*")
                    && clip.getDescription().hasMimeType("text/plain"),
                    "Native image delivery preserves both real formats");
            try (var rows = transport.query(granted, uri, null, new Bundle(), null)) {
                check(rows != null && rows.moveToFirst() && rows.getCount() == 1
                        && rows.getLong(rows.getColumnIndexOrThrow(OpenableColumns.SIZE)) == image.length,
                        "A paste target can query metadata on the granted image");
            }
            rejects(SecurityException.class, () -> transport.query(denied, uri, null, new Bundle(), null));
            rejects(SecurityException.class, () -> transport.openFile(granted, uri, "w", null));
            if (binder) {
                try (var first = new ParcelFileDescriptor.AutoCloseInputStream(
                                transport.openFile(granted, uri, "r", null));
                     var other = new ParcelFileDescriptor.AutoCloseInputStream(
                                transport.openFile(granted, uri, "r", null))) {
                    check(first.read() == 'B' && other.read() == 'B',
                            "Two target reads get independent offsets");
                    h.write("replace granted image");
                    check(java.util.Arrays.equals(first.readAllBytes(), new byte[]{'M', 0, 1}),
                            "Already opened Windows image snapshots survive clipboard replacement");
                }
            } else h.write("replace granted image");
            rejects(SecurityException.class, () -> transport.openFile(granted, uri, "r", null));
        });
        check(!grants.canAccess(h.privileged, uri, read | write, 0)
                        && !grants.canAccess(h.privileged, uri, read, 0),
                "Replacing an image retires its grant");
        h.as(h.peer, () -> rejects(SecurityException.class,
                () -> transport.query(denied, uri, null, new Bundle(), null)));
    }

    private static void clipboardUriGrants(ClipboardFixture h) throws Exception {
        Uri item = Uri.parse("content://" + OWNER + ".files/item");
        Uri data = Uri.parse("content://" + OWNER + ".files/intent");
        Uri sibling = Uri.parse("content://" + OWNER + ".files/other");
        int read = Intent.FLAG_GRANT_READ_URI_PERMISSION;
        int write = Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
        UriGrants grants = (UriGrants) call(h.atm, "uriGrants", new Class<?>[0]);
        ClipData content = new ClipData("private",
                new String[]{"text/uri-list"}, new ClipData.Item(null, null,
                        new Intent().setData(data).addFlags(write), item));
        h.write("kept until URI validation succeeds");
        int nativeSequence = h.nativeState.sequence;
        h.as(h.privileged, () -> rejects(SecurityException.class,
                () -> h.api.setPrimaryClipAsPackage(content, PRIVILEGED, null, 0, 0, OWNER)));
        check(h.nativeState.sequence == nativeSequence, "Rejected clip changed the native clipboard");
        h.asOwner(() -> h.api.setPrimaryClip(content, OWNER, null, 0, 0));
        h.as(h.privileged, () -> {
            check(h.api.hasPrimaryClip(PRIVILEGED, null, 0, 0)
                            && h.api.getPrimaryClipDescription(PRIVILEGED, null, 0, 0) != null,
                    "Clipboard metadata disappeared before the read");
            check(!grants.canAccess(h.privileged, item, read, 0)
                            && !grants.canAccess(h.privileged, data, read, 0),
                    "A metadata query granted URI access");
        });
        h.nativeState.foreground = null;
        h.as(h.peer, () -> check(h.api.getPrimaryClip(PEER, null, 0, 0) == null,
                "Background peer read the clipboard"));
        check(!grants.canAccess(h.peer, item, read, 0), "Denied clipboard read still granted the URI");
        h.as(h.privileged, () -> {
            ClipData delivered = h.api.getPrimaryClip(PRIVILEGED, null, 0, 0);
            check(delivered != null && item.equals(delivered.getItemAt(0).getUri())
                            && data.equals(delivered.getItemAt(0).getIntent().getData()),
                    "Clipboard lost the typed URI or its item Intent data");
        });
        for (Uri uri : List.of(item, data)) {
            check(grants.canAccess(h.privileged, uri, read, 0), "Clipboard did not grant read access");
            check(!grants.canAccess(h.privileged, uri, write, 0), "Clipboard granted write access");
        }
        check(!grants.canAccess(h.privileged, sibling, read, 0), "Clipboard grant covered a sibling URI");
        h.nativeState.external("external replaces URI clip", null);
        check(!grants.canAccess(h.privileged, item, read, 0)
                        && !grants.canAccess(h.privileged, data, read, 0),
                "External clipboard replacement retained prior URI grants");
        h.asOwner(() -> h.api.setPrimaryClip(content, OWNER, null, 0, 0));
        h.as(h.privileged, () -> check(h.api.getPrimaryClip(PRIVILEGED, null, 0, 0) != null,
                "A new URI clip could not be read after replacement"));
        h.asOwner(() -> h.api.clearPrimaryClip(OWNER, null, 0, 0));
        check(!grants.canAccess(h.privileged, item, read, 0)
                        && !grants.canAccess(h.privileged, data, read, 0),
                "Explicit clipboard clear retained prior URI grants");
    }

    private static void retireDelegatedUriPublisher(ClipboardFixture h) throws Exception {
        Uri uri = Uri.parse("content://" + OWNER + ".files/delegated");
        UriGrants grants = (UriGrants) call(h.atm, "uriGrants", new Class<?>[0]);
        int read = Intent.FLAG_GRANT_READ_URI_PERMISSION;
        h.asOwner(() -> call(grants, "grant", new Class<?>[]{String.class, Uri.class, int.class, int.class},
                PRIVILEGED, uri, read, 0));
        h.as(h.privileged, () -> h.api.setPrimaryClip(ClipData.newRawUri("delegated", uri),
                PRIVILEGED, null, 0, 0));
        h.configureIme(PEER + "/.Keyboard");
        try {
            h.as(h.peer, () -> {
                ClipData clip = h.api.getPrimaryClip(PEER, null, 0, 0);
                check(clip != null && uri.equals(clip.getItemAt(0).getUri()),
                        "Authorized clipboard publisher can delegate another provider's URI");
            });
            check(grants.canAccess(h.peer, uri, read, 0), "Clipboard recipient initially holds the delegated URI");
            int sequence = h.nativeState.sequence;
            call(h.pm, "beginPackageRemoval", new Class<?>[]{ApkPackage.class}, h.privileged);
            try {
                call(h.pm, "revokePackageCapabilities", new Class<?>[]{String.class}, PRIVILEGED);
                check(!grants.canAccess(h.peer, uri, read, 0),
                        "Publisher retirement revokes its delegated clipboard grant without another read");
                check(h.nativeState.sequence == sequence,
                        "Retiring a delegated URI has no native clipboard side effect");
            } finally {
                call(h.pm, "endPackageRemoval", new Class<?>[]{ApkPackage.class}, h.privileged);
            }
            h.as(h.peer, () -> check(h.api.getPrimaryClip(PEER, null, 0, 0) == null,
                    "A restored installation cannot revive the retired clipboard grant"));
        } finally {
            h.configureIme(null);
        }
    }

    @SuppressWarnings("unchecked") private static void callbackPolicyAndRetirement(ClipboardFixture h) throws Exception {
        Listener owner = new Listener(), peer = new Listener(), privileged = new Listener();
        h.nativeState.foreground = new long[]{PID, CREATED};
        h.add(h.owner, owner); h.add(h.peer, peer); h.add(h.privileged, privileged);
        h.write("callbacks");
        check(owner.calls.get() == 1 && peer.calls.get() == 0 && privileged.calls.get() == 1,
                "Saved listener identities, not ambient service authority, determine callbacks");
        h.nativeState.foreground = null; h.write("lost focus");
        check(owner.calls.get() == 1 && privileged.calls.get() == 2, "Focus is rechecked at dispatch");
        h.nativeState.lockState = 1; h.write("locked change");
        check(privileged.calls.get() == 3, "AOSP does not apply the getter lock gate to change callbacks");
        h.nativeState.lockState = 0;
        h.ops.setMode(OP_READ_CLIPBOARD, h.privileged.uid(), PRIVILEGED, MODE_IGNORED);
        h.write("appops callback denial"); check(privileged.calls.get() == 3, "Callback rechecks effective AppOps");
        h.ops.setMode(OP_READ_CLIPBOARD, h.privileged.uid(), PRIVILEGED, MODE_ALLOWED);
        Map<String, ApkPackage> packages = new LinkedHashMap<>((Map<String, ApkPackage>)get(h.pm, "mPackages"));
        ApkPackage replacement = pkg(PRIVILEGED, h.privileged.uid(), (byte)42, Manifest.permission.READ_CLIPBOARD_IN_BACKGROUND, Manifest.permission.SET_CLIP_SOURCE);
        packages.put(PRIVILEGED, replacement); set(h.pm, "mPackages", packages);
        read(h, h.privileged, false, "The old process cannot adopt its replacement's clipboard authority");
        Listener obsolete = new Listener();
        h.add(h.privileged, obsolete);
        check(obsolete.token.deaths.isEmpty(), "The old process was given a replacement-generation callback");
        int writes = h.nativeState.writes, clears = h.nativeState.clears;
        h.as(h.privileged, () -> {
            h.api.setPrimaryClip(ClipData.newPlainText("obsolete", "retired write"), PRIVILEGED, null, 0, 0);
            h.api.clearPrimaryClip(PRIVILEGED, null, 0, 0);
        });
        check(writes == h.nativeState.writes && clears == h.nativeState.clears,
                "An old process wrote or cleared the clipboard under the replacement's name");
        call(h.pm, "packageInstalled", new Class<?>[]{ApkPackage.class, boolean.class, Object.class}, replacement, true, new Object());
        long deadline = System.nanoTime() + java.util.concurrent.TimeUnit.SECONDS.toNanos(3);
        while (h.listeners().getRegisteredCallbackCount() == 3 && System.nanoTime() < deadline) Thread.sleep(5);
        check(h.listeners().getRegisteredCallbackCount() == 2 && privileged.token.deaths.isEmpty(), "Package replacement releases old-generation callback ownership");
        h.write("replacement"); check(privileged.calls.get() == 3, "New package generation cannot revive a saved listener");
        set(h.processes.get(2), "packageCode", replacement);
        read(h, replacement, true, "The newly admitted process keeps its normal clipboard access");
        h.as(replacement, () -> h.api.setPrimaryClip(ClipData.newPlainText("current", "new write"),
                PRIVILEGED, null, 0, 0));
        check("new write".equals(h.nativeState.text), "The newly admitted process retains write access");
        call(h.pm, "beginPackageRemoval", new Class<?>[]{ApkPackage.class}, h.owner);
        call(h.pm, "revokePackageCapabilities", new Class<?>[]{String.class}, OWNER);
        check(h.listeners().getRegisteredCallbackCount() == 1 && owner.token.deaths.isEmpty(), "Package removal revokes listener lifetime");
        call(h.pm, "endPackageRemoval", new Class<?>[]{ApkPackage.class}, h.owner);
        h.service.close();
        check(h.listeners().getRegisteredCallbackCount() == 0 && peer.token.deaths.isEmpty()
                && h.nativeState.listeners.isEmpty(), "Service close releases callback and native subscriptions");
        check(((Collection<?>)get(h.pm, "mPackageChanges")).size() == 1,
                "Closed clipboard releases its package-change subscription, leaving AppOps");
    }
    public static int run(boolean binder) throws Exception {
        checks = 0;
        try (ClipboardFixture h = new ClipboardFixture(binder)) {
            h.nativeState.sequence = -1; h.nativeState.text = "wrapped sequence";
            h.asOwner(() -> check("wrapped sequence".contentEquals(h.api.getPrimaryClip(OWNER, null, 0, 0).getItemAt(0).getText()),
                    "All nonzero unsigned Windows sequence values are valid"));
            h.nativeState.sequence = 1;
            focusAndIdentity(h); imeAndPermissions(h); appOps(h); sourceAndStorage(h);
            nativeImageGrants(h, binder); clipboardUriGrants(h); retireDelegatedUriPublisher(h);
            callbackPolicyAndRetirement(h);
        }
        return checks;
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        System.out.println("ClipboardSecurityContractTest all passed: " + run(true) + " production policy checks");
    }
}
