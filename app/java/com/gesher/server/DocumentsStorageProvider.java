/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.Manifest;
import android.app.ContentProviderHolder;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.pm.ApplicationInfo;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.CancellationSignal;
import android.os.FileUtils;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * The local filesystem behind the Windows document picker. The framework's
 * DocumentsProvider owns URI dispatch, tree enforcement and operation permissions;
 * ContentProvider.Transport owns Binder access. IDs are canonical guest paths.
 */
public final class DocumentsStorageProvider extends DocumentsProvider {
    public static final String AUTHORITY = "com.gesher.documents";
    private static final String ROOT_ID = "local";
    private static final String[] ROOT_COLUMNS = {Root.COLUMN_ROOT_ID, Root.COLUMN_FLAGS,
            Root.COLUMN_TITLE, Root.COLUMN_DOCUMENT_ID, Root.COLUMN_AVAILABLE_BYTES,
            Root.COLUMN_CAPACITY_BYTES, Root.COLUMN_MIME_TYPES};
    private static final String[] DOCUMENT_COLUMNS = {Document.COLUMN_DOCUMENT_ID,
            Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE, Document.COLUMN_FLAGS,
            Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED};

    private final ProviderInfo mInfo;
    private final UriGrants mGrants;
    private final CallerIdentity mCallers;
    final ContentProviders.RegisteredProvider registration;
    private static volatile libcore.content.type.MimeMap sMimeMap;

    DocumentsStorageProvider(ContentProviders providers, UriGrants grants) {
        mGrants = java.util.Objects.requireNonNull(grants);
        mCallers = providers.callers();
        mInfo = new ProviderInfo();
        ApplicationInfo app = new ApplicationInfo();
        app.packageName = "android";
        app.processName = "system";
        app.uid = Process.SYSTEM_UID;
        app.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_PERSISTENT;
        app.enabled = true;
        mInfo.applicationInfo = app;
        mInfo.packageName = app.packageName;
        mInfo.processName = app.processName;
        mInfo.name = getClass().getName();
        mInfo.authority = AUTHORITY;
        mInfo.enabled = true;
        mInfo.exported = true;
        mInfo.grantUriPermissions = true;
        mInfo.readPermission = Manifest.permission.MANAGE_DOCUMENTS;
        mInfo.writePermission = Manifest.permission.MANAGE_DOCUMENTS;
        mInfo.flags = ProviderInfo.FLAG_SINGLE_USER;
        // Register the actual instance before durable grants load at boot. Clients
        // only receive the transport through holder(), after real attachInfo.
        registration = new ContentProviders.RegisteredProvider(mInfo, getIContentProvider());
        providers.registerProvider(registration);
    }

    public ProviderInfo providerInfo() { return new ProviderInfo(mInfo); }

    synchronized ContentProviderHolder holder() {
        if (getContext() == null) attachTo(android.app.GesherSettingsContext.get());
        ContentProviderHolder result = new ContentProviderHolder(providerInfo());
        result.provider = getIContentProvider();
        result.noReleaseNeeded = true;
        return result;
    }

    /** Tests use this same attachInfo path with an explicit fixture context. */
    synchronized void attachTo(Context base) {
        if (getContext() != null) return;
        attachInfo(new ContextWrapper(java.util.Objects.requireNonNull(base)) {
            @Override public int checkUriPermission(Uri uri, int pid, int uid, int modes) {
                return mGrants.check(uri, pid, uid, modes, 0);
            }
            @Override public void grantUriPermission(String target, Uri uri, int modes) {
                mGrants.grantFromProvider(registration, target, uri, modes);
            }
            @Override public void revokeUriPermission(Uri uri, int modes) {
                mGrants.revokeFromProvider(registration, uri, modes);
            }
            @Override public void revokeUriPermission(String target, Uri uri, int modes) {
                mGrants.revokeFromProvider(registration, target, uri, modes);
            }
        }, mInfo);
    }

    @Override public boolean onCreate() { return true; }

    private void authenticate(android.content.AttributionSource source) {
        if (mCallers.isSystem()) return;
        if (source == null || source.getUid() != mCallers.callerUid()) {
            throw new SecurityException("Document attribution must identify its Binder caller");
        }
        mCallers.requirePackage(source.getPackageName());
    }

    @Override protected int enforceReadPermissionInner(Uri uri, android.content.AttributionSource source) {
        authenticate(source);
        return super.enforceReadPermissionInner(uri, source);
    }

    @Override protected int enforceWritePermissionInner(Uri uri, android.content.AttributionSource source) {
        authenticate(source);
        return super.enforceWritePermissionInner(uri, source);
    }

    static File file(String documentId) throws FileNotFoundException {
        if (documentId == null || !documentId.startsWith("/")) {
            throw new FileNotFoundException("Document ID is not an absolute guest path");
        }
        File file = new File(documentId);
        try {
            // A grant must not start naming another object after a symlink or
            // reparse-point substitution. Canonicalize before issuing a grant.
            if (!documentId.equals(file.getCanonicalPath())
                    || !(file.isFile() || file.isDirectory())) {
                throw new FileNotFoundException("Document is absent or is not a canonical file");
            }
            return file;
        } catch (IOException error) { throw missing("Cannot resolve document", error); }
    }

    static File selectedFile(String nativePath) throws FileNotFoundException {
        if (nativePath == null || nativePath.isEmpty()) throw new FileNotFoundException("No selection");
        try {
            String path = Zygote.fromWindowsPath(nativePath);
            if (!new File(path).isAbsolute()) throw new FileNotFoundException("Relative selection");
            return file(new File(path).getCanonicalPath());
        } catch (IOException | IllegalArgumentException error) { throw missing("Invalid selection", error); }
    }

    static libcore.content.type.MimeMap mimeMap() {
        libcore.content.type.MimeMap cached = sMimeMap;
        if (cached != null) return cached;
        libcore.content.type.MimeMap resolved = null;
        try {
            Object created = Class.forName("android.content.type.DefaultMimeMapFactory")
                    .getMethod("create").invoke(null);
            if (created instanceof libcore.content.type.MimeMap) {
                libcore.content.type.MimeMap full = (libcore.content.type.MimeMap) created;
                libcore.content.type.MimeMap.setDefaultSupplier(() -> full);
                resolved = full;
            }
        } catch (ReflectiveOperationException | LinkageError | RuntimeException ignored) { }
        if (resolved == null) {
            libcore.content.type.MimeMap base = libcore.content.type.MimeMap.getDefault();
            libcore.content.type.MimeMap.Builder builder = base.buildUpon();
            String[][] mediaFallback = {
                    {"image/jpeg", "jpeg"}, {"image/png", "png"}, {"image/gif", "gif"},
                    {"image/webp", "webp"}, {"image/heif", "heif"}, {"image/heic", "heic"},
                    {"video/mp4", "mp4"}, {"video/mp4", "m4v"}, {"video/quicktime", "mov"},
                    {"video/webm", "webm"}, {"video/x-matroska", "mkv"}, {"video/3gpp", "3gp"}
            };
            for (String[] entry : mediaFallback) {
                if (!base.hasExtension(entry[1])) builder.addMimeMapping("?" + entry[0], entry[1]);
            }
            resolved = builder.build();
        }
        sMimeMap = resolved;
        return resolved;
    }

    static String mimeType(File file) {
        if (file.isDirectory()) return Document.MIME_TYPE_DIR;
        String name = file.getName();
        int dot = name.lastIndexOf('.');
        String type = dot < 0 ? null : mimeMap()
                .guessMimeTypeFromExtension(name.substring(dot + 1).toLowerCase(Locale.ROOT));
        return type != null ? type : "application/octet-stream";
    }

    static int documentFlags(File file) {
        if (!(file.isFile() || file.isDirectory())) return 0;
        int flags = 0;
        if (file.isDirectory()) {
            if (file.canWrite()) flags |= Document.FLAG_DIR_SUPPORTS_CREATE;
            if (DocumentPicker.isBlockedTreeRoot(file)) flags |= Document.FLAG_DIR_BLOCKS_OPEN_DOCUMENT_TREE;
        } else if (file.canWrite()) flags |= Document.FLAG_SUPPORTS_WRITE;
        File parent = file.getParentFile();
        if (!isFilesystemRoot(file) && file.canWrite()
                && parent != null && parent.isDirectory() && parent.canWrite()) {
            flags |= Document.FLAG_SUPPORTS_DELETE | Document.FLAG_SUPPORTS_RENAME;
        }
        return flags;
    }

    private static boolean isFilesystemRoot(File file) {
        return file.getParentFile() == null || file.getPath().matches("/mnt/[A-Za-z]")
                || file.getPath().matches("/mnt/unc/[^/]+/[^/]+");
    }

    private static void row(MatrixCursor result, File file) {
        MatrixCursor.RowBuilder row = result.newRow();
        for (String column : result.getColumnNames()) {
            switch (column) {
                case Document.COLUMN_DOCUMENT_ID: row.add(file.getPath()); break;
                case Document.COLUMN_DISPLAY_NAME:
                    row.add(file.getName().isEmpty() ? file.getPath() : file.getName()); break;
                case Document.COLUMN_MIME_TYPE: row.add(mimeType(file)); break;
                case Document.COLUMN_FLAGS: row.add(documentFlags(file)); break;
                case Document.COLUMN_SIZE: row.add(file.isFile() ? file.length() : null); break;
                case Document.COLUMN_LAST_MODIFIED: row.add(file.lastModified()); break;
                default: row.add(null);
            }
        }
    }

    @Override public Cursor queryRoots(String[] projection) throws FileNotFoundException {
        MatrixCursor result = new MatrixCursor(projection == null ? ROOT_COLUMNS : projection);
        File root = file("/");
        if (!root.isDirectory() || !root.canRead()) return result;
        MatrixCursor.RowBuilder row = result.newRow();
        for (String column : result.getColumnNames()) {
            switch (column) {
                case Root.COLUMN_ROOT_ID: row.add(ROOT_ID); break;
                // This aggregate root includes network mounts; locality is a
                // property of each resolved selection, not of the whole root.
                case Root.COLUMN_FLAGS: row.add(Root.FLAG_SUPPORTS_IS_CHILD
                        | (root.canWrite() ? Root.FLAG_SUPPORTS_CREATE : 0)); break;
                case Root.COLUMN_TITLE: row.add("Windows files"); break;
                case Root.COLUMN_DOCUMENT_ID: row.add(root.getPath()); break;
                // The guest root also contains mounts; it is not a single volume.
                case Root.COLUMN_AVAILABLE_BYTES: case Root.COLUMN_CAPACITY_BYTES: row.add(null); break;
                case Root.COLUMN_MIME_TYPES: row.add("*/*"); break;
                default: row.add(null);
            }
        }
        return result;
    }

    @Override public Cursor queryDocument(String documentId, String[] projection)
            throws FileNotFoundException {
        MatrixCursor result = new MatrixCursor(projection == null ? DOCUMENT_COLUMNS : projection);
        row(result, file(documentId));
        return result;
    }

    @Override public Cursor queryChildDocuments(String parentId, String[] projection, String sortOrder)
            throws FileNotFoundException {
        File parent = file(parentId);
        if (!parent.isDirectory()) throw new FileNotFoundException("Document is not a directory");
        File[] children = parent.listFiles();
        if (children == null) throw new FileNotFoundException("Cannot list document children");
        MatrixCursor result = new MatrixCursor(projection == null ? DOCUMENT_COLUMNS : projection);
        if (DocumentPicker.blockedAndroidDirectory(parent)) return result;
        for (File child : children) {
            if (DocumentPicker.blockedAndroidDirectory(child)) continue;
            try { row(result, file(child.getAbsolutePath())); }
            catch (FileNotFoundException absentOrLink) { /* Never fabricate a disappearing child. */ }
        }
        return result;
    }

    @Override public boolean isChildDocument(String parentId, String childId) {
        try {
            Path parent = file(parentId).toPath(), child = file(childId).toPath();
            return !parent.equals(child) && child.startsWith(parent);
        } catch (FileNotFoundException absent) { return false; }
    }

    @Override public DocumentsContract.Path findDocumentPath(String parentId, String childId)
            throws FileNotFoundException {
        File child = file(childId), parent = file(parentId == null ? "/" : parentId);
        if (!child.equals(parent) && !isChildDocument(parent.getPath(), child.getPath())) {
            throw new FileNotFoundException("Document is outside the requested root");
        }
        List<String> path = new ArrayList<>();
        for (File current = child; current != null; current = current.getParentFile()) {
            path.add(0, current.getPath());
            if (current.equals(parent)) break;
        }
        return new DocumentsContract.Path(parentId == null ? ROOT_ID : null, path);
    }

    @Override public ParcelFileDescriptor openDocument(String documentId, String mode,
            CancellationSignal signal) throws FileNotFoundException {
        if (signal != null) signal.throwIfCanceled();
        File file = file(documentId);
        if (!file.isFile()) throw new FileNotFoundException("Only regular documents can be opened");
        int parsed = ParcelFileDescriptor.parseMode(mode);
        // A document disappearing after lookup must not be silently recreated.
        return ParcelFileDescriptor.open(file, parsed & ~ParcelFileDescriptor.MODE_CREATE);
    }

    private static void displayName(String name) {
        if (name == null || name.isEmpty() || name.equals(".") || name.equals("..")
                || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.indexOf('\0') >= 0) {
            throw new IllegalArgumentException("Invalid document display name");
        }
    }

    @Override public String createDocument(String parentId, String mimeType, String name)
            throws FileNotFoundException {
        displayName(name);
        if (mimeType == null || mimeType.isEmpty()) throw new IllegalArgumentException("Missing MIME type");
        File parent = file(parentId);
        if (!parent.isDirectory() || !parent.canWrite()) throw new FileNotFoundException("Parent is not writable");
        // Android chooses MIME extensions and collision names. Atomic creation
        // closes the final name-allocation race and never truncates an existing file.
        for (int attempt = 0; attempt < 32; attempt++) {
            File target = FileUtils.buildUniqueFile(parent, mimeType, FileUtils.buildValidFatFilename(name));
            try {
                if (Document.MIME_TYPE_DIR.equals(mimeType)) Files.createDirectory(target.toPath());
                else Files.createFile(target.toPath());
                return file(target.getCanonicalPath()).getPath();
            } catch (java.nio.file.FileAlreadyExistsException collision) {
                // Another creator won; choose another Android unique name.
            } catch (IOException error) { throw missing("Cannot create document", error); }
        }
        throw new FileNotFoundException("Cannot allocate an unused document name");
    }

    @Override public String renameDocument(String documentId, String name) throws FileNotFoundException {
        displayName(name);
        File source = file(documentId), parent = source.getParentFile();
        if (isFilesystemRoot(source)) throw new FileNotFoundException("Cannot rename a volume root");
        File target = new File(parent, FileUtils.buildValidFatFilename(name));
        if (source.equals(target)) return null;
        try {
            Files.move(source.toPath(), target.toPath()); // No REPLACE_EXISTING.
            return file(target.getCanonicalPath()).getPath();
        } catch (IOException error) { throw missing("Cannot rename document", error); }
    }

    @Override public void deleteDocument(String documentId) throws FileNotFoundException {
        File file = file(documentId);
        if (isFilesystemRoot(file)) throw new FileNotFoundException("Cannot delete a volume root");
        try { deleteTree(file); }
        catch (IOException error) { throw missing("Cannot delete document", error); }
    }

    private void deleteTree(File file) throws IOException {
        file = file(file.getPath()); // Refuse symlinks/reparse aliases outside the tree.
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children == null) throw new IOException("Cannot list directory for deletion");
            for (File child : children) {
                deleteTree(child);
                // DocumentsProvider retires the requested document after return;
                // retire each side-effect deletion now, even if a later delete fails.
                revokeDocumentPermission(child.getPath());
            }
        }
        Files.delete(file.toPath());
    }

    private static FileNotFoundException missing(String message, Exception error) {
        FileNotFoundException failure = new FileNotFoundException(message + ": " + error.getMessage());
        failure.initCause(error); return failure;
    }
}
