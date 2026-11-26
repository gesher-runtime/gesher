/* SPDX-License-Identifier: Apache-2.0 */
package org.gesher.contracts.storage;

import android.content.Context;
import dalvik.system.DexClassLoader;
import dalvik.system.InMemoryDexClassLoader;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;

final class DexContract {
    private static final String PLUGIN = "org.gesher.contracts.storage.plugin.Plugin";

    static void observe(Context context, JSONObject checks) throws Exception {
        ClassLoader parent = context.getClassLoader();
        byte[] dex;
        try (InputStream asset = context.getAssets().open("plugin.dex")) {
            dex = asset.readAllBytes();
        }
        File jar = File.createTempFile("plugin-", ".jar", context.getCodeCacheDir());
        try {
            try (FileOutputStream output = new FileOutputStream(jar);
                 InputStream asset = context.getAssets().open("plugin.jar")) {
                // Android 14+ requires app-written DEX containers to be read-only
                // before writing the bytes through the previously opened handle.
                if (!jar.setReadOnly()) {
                    throw new IOException("Could not mark DEX archive read-only");
                }
                byte[] chunk = new byte[4096];
                int count;
                while ((count = asset.read(chunk)) != -1) {
                    output.write(chunk, 0, count);
                }
            }
            ClassLoader memoryLoader = new InMemoryDexClassLoader(
                    ByteBuffer.wrap(dex), parent);
            ClassLoader archiveLoader = new DexClassLoader(
                    jar.getPath(), context.getCodeCacheDir().getPath(), null, parent);
            Class<?> memoryClass = memoryLoader.loadClass(PLUGIN);
            Class<?> archiveClass = archiveLoader.loadClass(PLUGIN);
            ByteBuffer direct = ByteBuffer.allocateDirect(dex.length + 2);
            direct.put((byte) 0).put(dex).flip().position(1);
            ClassLoader directLoader = new InMemoryDexClassLoader(direct, parent);
            Class<?> directClass = directLoader.loadClass(PLUGIN);
            Method memoryNext = memoryClass.getMethod("next");
            Method archiveNext = archiveClass.getMethod("next");
            checks.put("dex_parent_cannot_resolve_plugin", missing(parent));
            checks.put("dex_memory_loader_executes", memoryClass.getClassLoader() == memoryLoader
                    && Integer.valueOf(1).equals(memoryNext.invoke(null))
                    && Integer.valueOf(2).equals(memoryNext.invoke(null)));
            checks.put("dex_archive_loader_executes", archiveClass.getClassLoader() == archiveLoader
                    && Integer.valueOf(1).equals(archiveNext.invoke(null))
                    && Integer.valueOf(2).equals(archiveNext.invoke(null)));
            checks.put("dex_direct_slice_executes", directClass.getClassLoader() == directLoader
                    && Integer.valueOf(1).equals(directClass.getMethod("next").invoke(null)));
            checks.put("dex_loaders_have_distinct_class_identity",
                    memoryClass != archiveClass && directClass != memoryClass && directClass != archiveClass);
            checks.put("dex_invalid_memory_does_not_resolve",
                    missing(new InMemoryDexClassLoader(ByteBuffer.wrap(new byte[32]), parent)));
        } finally {
            // A loader can retain the backing file on Windows for its lifetime.
            // Keeping it in code cache lets the runtime/user reclaim it afterwards.
            jar.delete();
        }
    }

    private static boolean missing(ClassLoader loader) {
        try {
            loader.loadClass(PLUGIN);
            return false;
        } catch (ClassNotFoundException expected) {
            return true;
        }
    }
}
