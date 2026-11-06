/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.content.Context;
import android.content.ContextWrapper;
import android.os.DeviceIdleManager;
import android.os.IBinder;
import android.os.IDeviceIdleController;
import android.os.PowerExemptionManager;

import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.Collections;

/** No server or device changes: exercises the real managers and generated Binder stub. */
public final class DeviceIdleContractTest {
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        Class<?> type = Class.forName("com.gesher.server.DeviceIdleController");
        Constructor<?> constructor = type.getDeclaredConstructor();
        constructor.setAccessible(true);
        IDeviceIdleController impl = (IDeviceIdleController) constructor.newInstance();
        Class<?> standIn = Class.forName("com.gesher.server.StandIn");
        standIn.getMethod("setStrict", boolean.class).invoke(null, true);
        Object wrapper = standIn.getMethod("of", Class.class, android.os.IInterface.class)
                .invoke(null, IDeviceIdleController.class, impl);
        IBinder binder = (IBinder) standIn.getMethod("asBinder").invoke(wrapper);
        require(binder.getClass().getName().endsWith("IDeviceIdleControllerStub"),
                "deviceidle has no generated cross-process stub");
        IDeviceIdleController service = IDeviceIdleController.Stub.asInterface(binder);
        DeviceIdleManager idle = new DeviceIdleManager(null, service);
        Context context = new ContextWrapper(null) {
            @Override public String getSystemServiceName(Class<?> c) {
                return c == DeviceIdleManager.class ? Context.DEVICE_IDLE_CONTROLLER : null;
            }
            @Override public Object getSystemService(String name) {
                return Context.DEVICE_IDLE_CONTROLLER.equals(name) ? idle : null;
            }
        };
        // This is the constructor that dereferenced a null DeviceIdleManager in Phone Doctor.
        PowerExemptionManager power = new PowerExemptionManager(context);
        require(idle.getSystemPowerWhitelist().length == 0, "unexpected system whitelist");
        require(idle.getSystemPowerWhitelistExceptIdle().length == 0,
                "unexpected except-idle whitelist");
        for (Method method : IDeviceIdleController.class.getDeclaredMethods()) {
            String name = method.getName();
            if (method.getParameterCount() == 0) {
                Object result = method.invoke(service);
                require(result != null && java.lang.reflect.Array.getLength(result) == 0,
                        name + " must return an empty array");
            } else if (name.startsWith("isPower")) {
                for (String pkg : new String[] {"gesher.services", "missing.package", "", null}) {
                    require(Boolean.FALSE.equals(method.invoke(service, pkg)),
                            name + " invented allowlist membership");
                }
            } else {
                Object[] values = new Object[method.getParameterCount()];
                Class<?>[] parameters = method.getParameterTypes();
                for (int i = 0; i < values.length; i++) {
                    if (parameters[i] == int.class) values[i] = 0;
                    else if (parameters[i] == long.class) values[i] = 10_000L;
                    else if (parameters[i] == java.util.List.class) {
                        values[i] = Collections.singletonList("gesher.services");
                    } else values[i] = "gesher.services";
                }
                try {
                    method.invoke(service, values);
                    throw new AssertionError(name + " accepted an unprivileged administrator");
                } catch (InvocationTargetException e) {
                    require(e.getCause() instanceof SecurityException,
                            name + " failed with " + e.getCause());
                }
            }
        }
        // Re-query after denied writes: denial cannot publish permanent or temporary state.
        require(service.getUserPowerWhitelist().length == 0
                && service.getAppIdTempWhitelist().length == 0, "denied write changed policy");
        require(power != null, "power exemption manager construction failed");
        System.out.println("DeviceIdleContractTest all passed (22 methods, real manager constructors)");
    }
}
