/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.provider;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.Executor;

/**
 * Placeholder for the configinfrastructure module's DeviceConfig, which framework.jar
 * reads its server-side flags through (autofill, input, window manager, ...) but does
 * not contain. Flags live in this process only: nothing has set one, so every read
 * answers the caller's default; {@link #setProperty} sets one and tells the namespace's
 * listeners. Only the members framework.jar calls are here.
 */
public final class DeviceConfig {
    private static final Map<String, Map<String, String>> sValues = new HashMap<>();
    private static final Map<OnPropertiesChangedListener, ListenerRecord> sListeners =
            new HashMap<>();

    private DeviceConfig() {
    }

    private static final class ListenerRecord {
        final String namespace;
        final Executor executor;

        ListenerRecord(String namespace, Executor executor) {
            this.namespace = namespace;
            this.executor = executor;
        }
    }

    public static String getProperty(String namespace, String name) {
        synchronized (sValues) {
            Map<String, String> values = sValues.get(namespace);
            return values == null ? null : values.get(name);
        }
    }

    public static Properties getProperties(String namespace, String... names) {
        Map<String, String> result = new HashMap<>();
        synchronized (sValues) {
            Map<String, String> values = sValues.get(namespace);
            if (values != null) {
                if (names.length == 0) {
                    result.putAll(values);
                } else {
                    for (String name : names) {
                        if (values.containsKey(name)) result.put(name, values.get(name));
                    }
                }
            }
        }
        return new Properties(namespace, result);
    }

    public static String getString(String namespace, String name, String defaultValue) {
        String value = getProperty(namespace, name);
        return value != null ? value : defaultValue;
    }

    public static boolean getBoolean(String namespace, String name, boolean defaultValue) {
        String value = getProperty(namespace, name);
        return value != null ? Boolean.parseBoolean(value) : defaultValue;
    }

    public static int getInt(String namespace, String name, int defaultValue) {
        try {
            String value = getProperty(namespace, name);
            return value != null ? Integer.parseInt(value) : defaultValue;
        } catch (NumberFormatException e) {
            return defaultValue;
        }
    }

    public static long getLong(String namespace, String name, long defaultValue) {
        try {
            String value = getProperty(namespace, name);
            return value != null ? Long.parseLong(value) : defaultValue;
        } catch (NumberFormatException e) {
            return defaultValue;
        }
    }

    public static float getFloat(String namespace, String name, float defaultValue) {
        try {
            String value = getProperty(namespace, name);
            return value != null ? Float.parseFloat(value) : defaultValue;
        } catch (NumberFormatException e) {
            return defaultValue;
        }
    }

    public static boolean setProperty(String namespace, String name, String value,
            boolean makeDefault) {
        synchronized (sValues) {
            sValues.computeIfAbsent(namespace, n -> new HashMap<>()).put(name, value);
        }
        notifyListeners(namespace, Collections.singletonMap(name, value));
        return true;
    }

    public static boolean setProperties(Properties properties) {
        synchronized (sValues) {
            sValues.computeIfAbsent(properties.getNamespace(), n -> new HashMap<>())
                    .putAll(properties.mValues);
        }
        notifyListeners(properties.getNamespace(), properties.mValues);
        return true;
    }

    public static boolean deleteProperty(String namespace, String name) {
        synchronized (sValues) {
            Map<String, String> values = sValues.get(namespace);
            if (values == null || values.remove(name) == null) return false;
        }
        notifyListeners(namespace, Collections.singletonMap(name, null));
        return true;
    }

    public static void resetToDefaults(int resetMode, String namespace) {
        synchronized (sValues) {
            if (namespace == null) {
                sValues.clear();
            } else {
                sValues.remove(namespace);
            }
        }
    }

    public static void addOnPropertiesChangedListener(String namespace, Executor executor,
            OnPropertiesChangedListener listener) {
        synchronized (sListeners) {
            sListeners.put(listener, new ListenerRecord(namespace, executor));
        }
    }

    public static void removeOnPropertiesChangedListener(OnPropertiesChangedListener listener) {
        synchronized (sListeners) {
            sListeners.remove(listener);
        }
    }

    private static void notifyListeners(String namespace, Map<String, String> changed) {
        List<Map.Entry<OnPropertiesChangedListener, ListenerRecord>> targets = new ArrayList<>();
        synchronized (sListeners) {
            for (Map.Entry<OnPropertiesChangedListener, ListenerRecord> e : sListeners.entrySet()) {
                if (e.getValue().namespace.equals(namespace)) targets.add(e);
            }
        }
        Properties properties = new Properties(namespace, new HashMap<>(changed));
        for (Map.Entry<OnPropertiesChangedListener, ListenerRecord> e : targets) {
            e.getValue().executor.execute(() -> e.getKey().onPropertiesChanged(properties));
        }
    }

    /** Listens to a namespace's changes. */
    public interface OnPropertiesChangedListener {
        void onPropertiesChanged(Properties properties);
    }

    /** Receives DeviceConfig's access and update events (none happen here). */
    public interface MonitorCallback {
        void onNamespaceUpdate(String updatedNamespace);

        void onDeviceConfigAccess(String callingPackage, String namespace);
    }

    /** A bad configuration was set. */
    public static class BadConfigException extends Exception {
        public BadConfigException() {
        }
    }

    /** A namespace's values. */
    public static class Properties {
        private final String mNamespace;
        private final Map<String, String> mValues;

        public Properties(String namespace, Map<String, String> keyValueMap) {
            mNamespace = namespace;
            mValues = keyValueMap == null ? new HashMap<>() : new HashMap<>(keyValueMap);
        }

        public String getNamespace() {
            return mNamespace;
        }

        public Set<String> getKeyset() {
            return Collections.unmodifiableSet(mValues.keySet());
        }

        public String getString(String name, String defaultValue) {
            String value = mValues.get(name);
            return value != null ? value : defaultValue;
        }

        public boolean getBoolean(String name, boolean defaultValue) {
            String value = mValues.get(name);
            return value != null ? Boolean.parseBoolean(value) : defaultValue;
        }

        public int getInt(String name, int defaultValue) {
            try {
                String value = mValues.get(name);
                return value != null ? Integer.parseInt(value) : defaultValue;
            } catch (NumberFormatException e) {
                return defaultValue;
            }
        }

        public long getLong(String name, long defaultValue) {
            try {
                String value = mValues.get(name);
                return value != null ? Long.parseLong(value) : defaultValue;
            } catch (NumberFormatException e) {
                return defaultValue;
            }
        }

        public float getFloat(String name, float defaultValue) {
            try {
                String value = mValues.get(name);
                return value != null ? Float.parseFloat(value) : defaultValue;
            } catch (NumberFormatException e) {
                return defaultValue;
            }
        }
    }
}
