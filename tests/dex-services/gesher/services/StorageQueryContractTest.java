/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.ContentResolver;
import android.content.Context;
import android.content.ContextWrapper;
import android.os.Binder;
import android.os.Environment;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.storage.IStorageManager;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.os.storage.VolumeInfo;

import com.gesher.server.Services;
import com.gesher.server.StandIn;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.lang.reflect.Field;
import java.nio.file.Files;
import java.util.List;
import java.util.Map;
import java.util.UUID;

/** Production mount service -> generated AIDL/Parcel -> real StorageManager queries. */
public final class StorageQueryContractTest {
    private static int checks;

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        ++checks;
    }

    private interface Call { void run() throws Exception; }

    private static void unavailable(Call call, String message) throws Exception {
        try { call.run(); throw new AssertionError(message); }
        catch (IOException expected) { ++checks; }
    }

    private static void queries(StorageManager client, IStorageManager wire,
            Services.StorageService local) throws Exception {
        File data = Environment.getDataDirectory().getCanonicalFile();
        File sharedPath = new File(data, "media/0");
        require(client.getDisks().isEmpty(), "invented Android-managed disks");
        require(client.getVolumeRecords().isEmpty(), "invented remembered volumes");
        require(client.findDiskById("missing") == null, "unknown disk appeared present");
        require(client.findRecordByUuid("missing") == null, "unknown record appeared present");

        List<VolumeInfo> volumes = client.getVolumes();
        require(volumes.size() == 2, "private/shared inventory is incomplete");
        VolumeInfo internal = client.findVolumeById(VolumeInfo.ID_PRIVATE_INTERNAL);
        require(internal != null && internal.type == VolumeInfo.TYPE_PRIVATE
                        && internal.isMountedWritable()
                        && data.equals(internal.getPath().getCanonicalFile()),
                "internal storage metadata disagrees with ANDROID_DATA");
        VolumeInfo shared = client.findVolumeById(VolumeInfo.ID_EMULATED_INTERNAL + ";0");
        require(shared != null && shared.type == VolumeInfo.TYPE_EMULATED
                        && shared.isPrimary() && shared.isMountedWritable()
                        && shared.isVisibleForRead(0) && shared.isVisibleForWrite(0)
                        && !shared.isVisibleForUser(10),
                "emulated volume visibility/state does not describe the desktop user");
        require(sharedPath.equals(shared.getPathForUser(0).getCanonicalFile()),
                "VolumeInfo appended the user ID to an already per-user path");
        require(client.findPrivateForEmulated(shared).id.equals(internal.id)
                        && client.findEmulatedForPrivate(internal).id.equals(shared.id),
                "private/emulated storage links are broken");
        require(data.equals(client.findPathForUuid(StorageManager.UUID_PRIVATE_INTERNAL)
                        .getCanonicalFile()), "default UUID did not resolve to private data");
        require(client.getWritablePrivateVolumes().size() == 1
                        && client.getWritablePrivateVolumes().get(0).id.equals(internal.id),
                "private writable inventory included the shared view or omitted data");
        require(client.findVolumeById("missing") == null
                        && client.findVolumeByUuid("missing") == null
                        && client.getPrimaryPhysicalVolume() == null,
                "invented an unknown or physical volume");

        require(StorageManager.UUID_DEFAULT.equals(client.getUuidForPath(data))
                        && StorageManager.UUID_DEFAULT.equals(client.getUuidForPath(
                                new File(sharedPath, "ordinary-file"))),
                "private and shared files lost their common storage UUID");
        File outside = new File(data.getParentFile(), data.getName() + "-unmanaged-storage");
        try {
            client.getUuidForPath(outside);
            throw new AssertionError("unmanaged path acquired an Android storage UUID");
        } catch (IOException expected) {
            ++checks;
        }
        // Before this repair the public path above threw NullPointerException while
        // iterating the null getVolumes array, instead of its documented IOException.

        StorageVolume[] exposed = wire.getVolumeList(0, "gesher.services", 0);
        require(exposed.length == 1, "shared volume missing from getVolumeList");
        StorageVolume primary = exposed[0];
        require(shared.id.equals(primary.getId()) && primary.isPrimary() && primary.isEmulated()
                        && !primary.isRemovable() && primary.getOwner().getIdentifier() == 0
                        && Environment.MEDIA_MOUNTED.equals(primary.getState())
                        && sharedPath.equals(primary.getDirectory().getCanonicalFile()),
                "StorageVolume disagrees with VolumeInfo after AIDL parceling");
        require(StorageManager.UUID_DEFAULT.equals(primary.getStorageUuid())
                        && primary.getUuid() == null,
                "emulated internal storage was described as an unadopted portable disk");
        for (int user : new int[] {-1, 1, 10, Integer.MAX_VALUE}) {
            require(wire.getVolumeList(user, "gesher.services", 0).length == 0,
                    "unknown desktop user acquired shared storage");
        }
        require(wire.getVolumes(Integer.MAX_VALUE).length == 2
                        && wire.getVolumeRecords(Integer.MAX_VALUE).length == 0,
                "query flags changed the fixed volume inventory");

        VolumeInfo[] first = local.getVolumes(0);
        first[0].path = "caller-modified";
        first[1].mountFlags = 0;
        first[1].state = VolumeInfo.STATE_UNMOUNTED;
        VolumeInfo[] next = local.getVolumes(0);
        require(next != first && data.equals(next[0].getPath().getCanonicalFile())
                        && next[1].isPrimary() && next[1].isMountedWritable(),
                "a local caller modified later service snapshots");
        first[0] = null;
        require(local.getVolumes(0)[0] != null, "service returned a shared inventory array");

        require(wire.isCeStorageUnlocked(0) && new Services.UserManager().isUserUnlocked(0),
                "mount and user services disagree on owner data availability");
        for (int user : new int[] {-1, 1, 10, Integer.MAX_VALUE}) {
            require(!wire.isCeStorageUnlocked(user), "absent user has unlocked credential storage");
        }

        long usable = data.getUsableSpace();
        long allocatable = client.getAllocatableBytes(StorageManager.UUID_DEFAULT);
        require(allocatable >= 0 && allocatable <= data.getTotalSpace(),
                "reported allocation capacity is outside the backing filesystem");
        if (usable > 1024L * 1024 * 1024) {
            require(allocatable > 0, "healthy backing storage reported no allocation capacity");
        }
        require(wire.getAllocatableBytes(StorageManager.UUID_PRIVATE_INTERNAL,
                        StorageManager.FLAG_ALLOCATE_CACHE_ONLY, "gesher.services") == 0,
                "unimplemented cache reclamation offered imaginary capacity");
        client.allocateBytes(StorageManager.UUID_DEFAULT, 0);
        if (allocatable > 0) client.allocateBytes(StorageManager.UUID_DEFAULT, 1);
        ++checks; // Successful capacity checks do not allocate or truncate a file.
        unavailable(() -> client.allocateBytes(StorageManager.UUID_DEFAULT, Long.MAX_VALUE),
                "impossible allocation silently succeeded");
        UUID missing = UUID.fromString("3aacdc2c-59d0-4a67-8c91-44baf7dd5e4c");
        unavailable(() -> client.getAllocatableBytes(missing), "unknown volume has capacity");
        unavailable(() -> client.allocateBytes(missing, 0), "unknown volume accepted allocation");
        try {
            client.allocateBytes(StorageManager.UUID_DEFAULT, -1);
            throw new AssertionError("negative allocation accepted");
        } catch (IllegalArgumentException expected) { ++checks; }
    }

    /** Missing or obstructed backing paths must not be advertised as mounted. */
    private static void backingStates() throws Exception {
        File base = Files.createTempDirectory(Environment.getDataDirectory().toPath(),
                "gesher-storage-state-").toFile();
        File data = new File(base, "data");
        File shared = new File(data, "media/0");
        java.lang.reflect.Constructor<Services.StorageService> constructor =
                Services.StorageService.class.getDeclaredConstructor(File.class);
        constructor.setAccessible(true);
        Services.StorageService service = constructor.newInstance(data);
        try {
            VolumeInfo[] absent = service.getVolumes(0);
            require(absent[0].state == VolumeInfo.STATE_UNMOUNTED
                            && absent[1].state == VolumeInfo.STATE_UNMOUNTED,
                    "missing backing directories were reported mounted");
            StorageVolume primary = service.getVolumeList(0, "gesher.services", 0)[0];
            require(Environment.MEDIA_UNMOUNTED.equals(primary.getState())
                            && primary.getDirectory() == null && !data.exists(),
                    "shared-storage query recreated a missing data volume or reported it mounted");
            unavailable(() -> {
                try {
                    service.getAllocatableBytes(StorageManager.UUID_PRIVATE_INTERNAL, 0,
                            "gesher.services");
                } catch (android.os.ParcelableException failure) {
                    failure.maybeRethrow(IOException.class);
                    throw failure;
                }
            }, "unavailable backing data offered allocation capacity");

            Files.write(data.toPath(), new byte[] {1});
            VolumeInfo[] obstructed = service.getVolumes(0);
            require(obstructed[0].state == VolumeInfo.STATE_UNMOUNTED
                            && obstructed[1].state == VolumeInfo.STATE_UNMOUNTED,
                    "ordinary file was treated as a mounted data directory");
            require(service.getVolumeList(0, "gesher.services", 0)[0].getDirectory() == null,
                    "failed shared-directory creation still exposed a mounted directory");
            Files.delete(data.toPath());
            Files.createDirectory(data.toPath());
            VolumeInfo[] present = service.getVolumes(0);
            require(present[0].isMountedWritable() && present[1].isMountedWritable()
                            && shared.isDirectory(),
                    "emulated shared storage was not created on the available backing volume");
            Files.delete(shared.toPath());
            Files.write(shared.toPath(), new byte[] {1});
            VolumeInfo[] blockedShared = service.getVolumes(0);
            require(blockedShared[0].isMountedWritable()
                            && blockedShared[1].state == VolumeInfo.STATE_UNMOUNTED,
                    "blocked shared path was mounted or changed private-volume state");
            require(Environment.MEDIA_UNMOUNTED.equals(service.getVolumeList(0,
                            "gesher.services", 0)[0].getState()),
                    "StorageVolume and VolumeInfo disagree on unavailable shared storage");
        } finally {
            Files.deleteIfExists(shared.toPath());
            Files.deleteIfExists(new File(data, "media").toPath());
            Files.deleteIfExists(data.toPath());
            Files.delete(base.toPath());
        }
    }

    /** Exercises the framework FD overload and actual native file growth on Windows. */
    private static void allocateFile(StorageManager client) throws Exception {
        File file = File.createTempFile("gesher-allocation-", ".bin", Environment.getDataDirectory());
        try (RandomAccessFile contents = new RandomAccessFile(file, "rw")) {
            long marker = 0x6765736865722026L;
            contents.writeLong(marker);
            require(client.isAllocationSupported(contents.getFD()),
                    "opened private file was not recognized as allocation-capable storage");
            client.allocateBytes(contents.getFD(), 64 * 1024);
            require(contents.length() >= 64 * 1024, "FD allocation did not grow the real file");
            contents.seek(0);
            require(contents.readLong() == marker, "FD allocation lost existing file contents");
            contents.seek(64 * 1024 - 1);
            require(contents.read() == 0, "allocated tail was not zero-initialized");
        } finally {
            java.nio.file.Files.deleteIfExists(file.toPath());
        }
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        if (Looper.myLooper() == null) Looper.prepareMainLooper();
        Services.StorageService local = new Services.StorageService();
        StandIn.setStrict(true);
        IBinder target = StandIn.of(IStorageManager.class, local).asBinder();
        // Deliberately omit an attached local interface to exercise both Parcel
        // directions rather than the generated Stub's same-process shortcut.
        IBinder remote = new Binder() {
            @Override protected boolean onTransact(int code, Parcel request, Parcel reply, int flags)
                    throws RemoteException {
                return target.transact(code, request, reply, flags);
            }
        };
        IStorageManager wire = IStorageManager.Stub.asInterface(remote);
        Field field = ServiceManager.class.getDeclaredField("sCache");
        field.setAccessible(true);
        @SuppressWarnings("unchecked")
        Map<String, IBinder> cache = (Map<String, IBinder>) field.get(null);
        IBinder previous = cache.put("mount", remote);
        try {
            // These queries do not require an app or a ContentResolver. The manager,
            // its service interface and all parcelable value types are real framework code.
            Context context = new ContextWrapper(null) {
                @Override public ContentResolver getContentResolver() { return null; }
                @Override public String getSystemServiceName(Class<?> type) { return null; }
                @Override public int getUserId() { return 0; }
                @Override public String getOpPackageName() { return "gesher.services"; }
            };
            StorageManager client = new StorageManager(context, Looper.myLooper());
            queries(client, wire, local);
            backingStates();
            require(StorageManager.isCeStorageUnlocked(0) && !StorageManager.isCeStorageUnlocked(10),
                    "framework CE-storage wrapper disagrees with the single-user state");
            java.lang.reflect.Method strictModeQuery = android.os.StrictMode.class
                    .getDeclaredMethod("isCeStorageUnlocked", int.class);
            strictModeQuery.setAccessible(true);
            require((Boolean) strictModeQuery.invoke(null, 0)
                            && !(Boolean) strictModeQuery.invoke(null, 10),
                    "StrictMode's credential-protected path detector sees the wrong unlock state");
            allocateFile(client);
        } finally {
            if (previous == null) cache.remove("mount");
            else cache.put("mount", previous);
            StandIn.setStrict(false);
        }
        System.out.println("StorageQueryContractTest all passed: " + checks);
    }
}
