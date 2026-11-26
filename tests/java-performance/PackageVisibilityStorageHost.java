/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import java.lang.reflect.Field;
import java.util.IdentityHashMap;
import java.util.Map;
import gesher.services.PackageVisibilityContractTest;

/** Retained metadata structure sizes; APK/parsed manifests, shared strings and native heaps are excluded. */
public final class PackageVisibilityStorageHost {
    private static Object field(Object owner, String name) throws Exception {
        for (Class<?> type = owner.getClass(); type != null; type = type.getSuperclass()) {
            try { Field field = type.getDeclaredField(name); field.setAccessible(true); return field.get(owner); }
            catch (NoSuchFieldException missing) { }
        }
        throw new NoSuchFieldException(name);
    }
    private static final class Sizes {
        final IdentityHashMap<Object, Boolean> seen = new IdentityHashMap<>();
        long add(Object value) {
            return value == null || seen.put(value, true) != null ? 0 : PackageMetadataSizeAgent.size(value);
        }
        long map(Map<?, ?> map, boolean ownedIntegerKeys) throws Exception {
            long total = add(map);
            Object[] table = (Object[])field(map, "table"); total += add(table);
            if (table != null) for (Object bucket : table) {
                for (Object node = bucket; node != null; node = field(node, "next")) {
                    total += add(node);
                    if (ownedIntegerKeys) total += add(((Map.Entry<?, ?>)node).getKey());
                }
            }
            return total;
        }
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        int packages = Integer.parseInt(args[1]); boolean grouped = Boolean.parseBoolean(args[2]);
        Object[] values = PackageVisibilityContractTest.uidMetadataStorage(packages, grouped);
        Object snapshot = values[0], visibility = values[1];
        Map<?, ?> names = (Map<?, ?>)field(visibility, "mPackages");
        Object groups = field(visibility, "mPackagesByAppId");
        Sizes sizes = new Sizes();
        long index; int capacity, unique;
        if (groups instanceof Map<?, ?> map) {
            index = sizes.map(map, true);
            Object[] table = (Object[])field(map, "table"); capacity = table == null ? 0 : table.length;
            unique = map.size();
        } else {
            Object heads = field(groups, "heads");
            index = sizes.add(groups) + sizes.add(field(groups, "keys")) + sizes.add(heads);
            capacity = java.lang.reflect.Array.getLength(heads); unique = (Integer)field(groups, "size");
        }
        long structure = sizes.add(visibility) + index + sizes.map(names, false);
        for (Object entry : names.values()) structure += sizes.add(entry);
        Object protectedSet = field(visibility, "mProtectedBroadcasts");
        structure += sizes.add(protectedSet) + sizes.map((Map<?, ?>)field(protectedSet, "map"), false);
        structure += sizes.add(field(visibility, "mPlatformProtectedBroadcast"));
        System.out.println("{\"packages\":" + packages + ",\"grouped\":" + grouped
                + ",\"definition_entries\":" + names.size() + ",\"app_id_groups\":" + unique
                + ",\"index_capacity\":" + capacity + ",\"index_structure_bytes\":" + index
                + ",\"visibility_structure_bytes\":" + structure
                + ",\"snapshot_shallow_bytes\":" + sizes.add(snapshot) + "}");
    }
}
