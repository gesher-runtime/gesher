/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.location.Country;
import android.location.CountryDetector;
import android.location.ICountryDetector;
import android.os.IBinder;

import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.Locale;

/**
 * country_detector as an app reaches it: the production CountryDetectorService with its
 * Windows backend (WindowsCountry), served through StandIn and the generated
 * ICountryDetector stub, read with android.location.CountryDetector. The country is this
 * machine's Windows "Country or region" (WindowsHost.homeRegion) or, without one, the
 * default Locale's country, with source COUNTRY_SOURCE_LOCALE.
 * Prints "CountryDetectorTest all passed".
 */
public final class CountryDetectorTest {
    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        Class<?> host = Class.forName("com.gesher.server.WindowsHost");
        String windows = (String) host.getMethod("homeRegion").invoke(null);
        Class<?> backendClass = Class.forName("com.gesher.server.WindowsCountry");
        Method create = backendClass.getDeclaredMethod("create");
        create.setAccessible(true);
        Object backend = create.invoke(null);
        require(backend != null, "no Windows backend although the natives are loaded");
        Class<?> serviceClass = Class.forName("com.gesher.server.CountryDetectorService");
        Constructor<?> constructor = serviceClass.getConstructor(
                Class.forName("com.gesher.server.CountryDetectorService$Backend"));
        ICountryDetector service = (ICountryDetector) constructor.newInstance(backend);
        Class<?> standIn = Class.forName("com.gesher.server.StandIn");
        Object wrapped = standIn.getMethod("of", Class.class, android.os.IInterface.class)
                .invoke(null, ICountryDetector.class, service);
        IBinder binder = (IBinder) standIn.getMethod("asBinder").invoke(wrapped);
        CountryDetector detector = new CountryDetector(ICountryDetector.Stub.asInterface(binder));
        Country country = detector.detectCountry();
        String expected = windows != null ? windows : Locale.getDefault().getCountry().toUpperCase(Locale.ROOT);
        require(country != null, "detectCountry returned null");
        require(expected.equals(country.getCountryCode()),
                "detectCountry " + country + ", Windows region " + windows);
        require(country.getSource() == Country.COUNTRY_SOURCE_LOCALE, "source " + country.getSource());
        System.out.println("CountryDetectorTest: detectCountry is " + country.getCountryCode()
                + " (" + (windows != null ? "Windows Country or region" : "no Windows region: the default Locale")
                + "), source COUNTRY_SOURCE_LOCALE");
        System.out.println("CountryDetectorTest all passed");
        System.out.flush();
    }
}
