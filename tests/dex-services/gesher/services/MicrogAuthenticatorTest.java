/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.accounts.AccountManager;
import android.accounts.AuthenticatorDescription;
import android.accounts.IAccountManager;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.util.DisplayMetrics;
import java.io.File;
import java.io.RandomAccessFile;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.util.Collection;
import java.util.Collections;
import java.util.List;
import org.xmlpull.v1.XmlPullParser;

/** Official APK authenticator discovery/signer regression. Never reads accounts or runs APK code. */
public final class MicrogAuthenticatorTest {
    private static final String GOOGLE = "com.google", GMS = "com.google.android.gms";
    private static final String CUSTOM = "android.accounts.AccountAuthenticator.customTokens";
    private static void require(boolean value, String message) { if (!value) throw new AssertionError(message); }
    private static Method method(Class<?> type, String name, Class<?>... parameters) throws Exception {
        Method m = type.getDeclaredMethod(name, parameters); m.setAccessible(true); return m;
    }
    private static Object construct(Class<?> type, Class<?>[] parameters, Object... values) throws Exception {
        Constructor<?> c = type.getDeclaredConstructor(parameters); c.setAccessible(true); return c.newInstance(values);
    }
    private static Object field(Object owner, String name) throws Exception {
        Field f = owner.getClass().getDeclaredField(name); f.setAccessible(true); return f.get(owner);
    }
    private static final Class<?> APK;
    static {
        try { APK = Class.forName("com.gesher.server.ApkPackage"); }
        catch (ClassNotFoundException e) { throw new ExceptionInInitializerError(e); }
    }
    private static Object parse(String path) throws Exception { return APK.getMethod("parse", String.class).invoke(null, path); }

    private static final class Registry {
        final Object packages;
        final IAccountManager accounts;
        final Class<?> managerType = Class.forName("com.gesher.server.AccountManagerService");
        Registry(Object apk) throws Exception {
            Class<?> pm = Class.forName("com.gesher.server.PackageManagerService");
            Class<?> am = Class.forName("com.gesher.server.ActivityManagerService");
            packages = construct(pm, new Class<?>[]{Collection.class}, Collections.singletonList(apk));
            accounts = (IAccountManager) construct(managerType, new Class<?>[]{am, pm}, null, packages);
        }
        @SuppressWarnings("unchecked") List<ResolveInfo> services() throws Exception {
            return (List<ResolveInfo>) method(packages.getClass(), "services", Intent.class, String.class, long.class)
                    .invoke(packages, new Intent(AccountManager.ACTION_AUTHENTICATOR_INTENT), null,
                            (long) PackageManager.GET_META_DATA);
        }
        AuthenticatorDescription descriptor(String type) throws Exception {
            AuthenticatorDescription result = null;
            for (AuthenticatorDescription d : accounts.getAuthenticatorTypes(0)) {
                if (!type.equals(d.type)) continue;
                require(result == null, "Duplicate authenticator descriptor for tested type");
                result = d;
            }
            return result;
        }
        ServiceInfo service(String suffix) throws Exception {
            for (ResolveInfo resolved : services()) {
                if (resolved.serviceInfo.name.endsWith(suffix)) return resolved.serviceInfo;
            }
            throw new AssertionError("Required public APK service fixture was not resolved: " + suffix);
        }
        boolean registered(ServiceInfo service) throws Exception {
            for (Object authenticator : (List<?>) method(managerType, "authenticators").invoke(accounts)) {
                if (new ComponentName(service.packageName, service.name).equals(field(authenticator, "service"))) return true;
            }
            return false;
        }
    }

    private static boolean xmlCustomTokens(ServiceInfo service) throws Exception {
        try (AssetManager assets = new AssetManager()) {
            require(assets.addAssetPath(service.applicationInfo.sourceDir) != 0, "Authenticator resources unavailable");
            DisplayMetrics metrics = new DisplayMetrics(); metrics.setToDefaults();
            Configuration configuration = new Configuration(); configuration.setToDefaults();
            Resources resources = new Resources(assets, metrics, configuration);
            try (XmlResourceParser xml = resources.getXml(service.metaData.getInt(AccountManager.AUTHENTICATOR_META_DATA_NAME))) {
                int event;
                do { event = xml.next(); } while (event != XmlPullParser.START_TAG && event != XmlPullParser.END_DOCUMENT);
                require(event == XmlPullParser.START_TAG && "account-authenticator".equals(xml.getName()),
                        "Invalid authenticator XML fixture");
                return xml.getAttributeBooleanValue("http://schemas.android.com/apk/res/android", "customTokens", false);
            }
        }
    }

    public static void main(String[] args) throws Exception {
        require(args.length == 2, "Pass the unmodified official GmsCore APK and fresh devsvc.apk (metadata only)");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false, System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        File isolated = Files.createTempDirectory("gesher-authenticator-metadata-").toFile();
        android.system.Os.setenv("ANDROID_DATA", isolated.getAbsolutePath(), true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true);

        Object official = parse(args[0]);
        require(GMS.equals(APK.getField("packageName").get(official)), "The official fixture is not GmsCore");
        require((Boolean) method(APK, "hasVerifiedMicrogSigner").invoke(official), "Official APK did not pass the actual microG signer/integrity pin");
        Registry real = new Registry(official);
        ServiceInfo google = real.service("GoogleLoginService");
        require(!google.exported && google.permission == null,
                "Official GoogleLoginService must exercise the private, unpermissioned service discovery path");
        require(google.metaData != null && xmlCustomTokens(google),
                "Official authenticator XML customTokens metadata missing");
        AuthenticatorDescription descriptor = real.descriptor(GOOGLE);
        require(descriptor != null && GMS.equals(descriptor.packageName) && descriptor.customTokens,
                "Verified official private Google authenticator was absent or lost customTokens");
        boolean matched = false;
        for (Object authenticator : (List<?>) method(real.managerType, "authenticators").invoke(real.accounts)) {
            if (GOOGLE.equals(field(authenticator, "type"))) {
                require(new ComponentName(GMS, google.name).equals(field(authenticator, "service")),
                        "Google descriptor selected a different service than the verified private login component");
                matched = true;
            }
        }
        require(matched, "Private GoogleLoginService was not registered");
        int disabledOfficial = 0;
        for (ResolveInfo resolved : real.services()) {
            ServiceInfo service = resolved.serviceInfo;
            if (!service.enabled) {
                disabledOfficial++;
                require(!real.registered(service), "Disabled official authenticator component entered the registry");
            }
        }
        require(disabledOfficial > 0, "Official APK must exercise its disabled authenticator component metadata");
        System.out.println("MicrogAuthenticatorTest: verified official private GoogleLoginService is discoverable with custom-token metadata");

        Registry fixture = new Registry(parse(args[1]));
        ServiceInfo privateService = fixture.service("TestPrivateAuthenticator");
        require(!privateService.exported && privateService.permission == null
                        && privateService.metaData.getBoolean(CUSTOM, false) && !xmlCustomTokens(privateService),
                "Private fixture must obtain customTokens only from service metadata");
        AuthenticatorDescription privateDescriptor = fixture.descriptor("com.gesher.devsvc.private");
        require(privateDescriptor != null && privateDescriptor.customTokens,
                "Private authenticator or service-level customTokens was lost");
        require(fixture.descriptor("com.gesher.devsvc.account") != null,
                "Exported permission-protected authenticator stopped registering");
        AuthenticatorDescription xmlCustom = fixture.descriptor("com.gesher.devsvc.custom");
        require(xmlCustom != null && xmlCustom.customTokens, "Existing XML customTokens compatibility changed");
        ServiceInfo unsafe = fixture.service("TestUnprotectedAuthenticator");
        require(unsafe.exported && unsafe.permission == null && fixture.descriptor("com.gesher.devsvc.unprotected") == null,
                "Exported unprotected authenticator entered the registry");
        ServiceInfo forged = fixture.service("TestForgedGoogleAuthenticator");
        require(!forged.exported && forged.permission == null && fixture.descriptor(GOOGLE) == null,
                "Another private package took ownership of the Google account type");
        ServiceInfo disabledService = fixture.service("TestDisabledAuthenticator");
        require(!disabledService.enabled && fixture.descriptor("com.gesher.devsvc.disabled") == null,
                "Disabled private authenticator entered the registry");
        System.out.println("MicrogAuthenticatorTest: private and exported permission-gated fixtures register; unsafe exports and forged Google owner are rejected; XML/service customTokens both work");

        Object disabledOwner = parse(args[1]);
        Object rawPackage = APK.getField("parsed").get(disabledOwner);
        rawPackage.getClass().getMethod("setEnabled", boolean.class).invoke(rawPackage, false);
        require(!(Boolean) rawPackage.getClass().getMethod("isEnabled").invoke(rawPackage),
                "Disabled-owner parsed-package fixture did not retain its manifest state");
        Registry disabledApp = new Registry(disabledOwner);
        ServiceInfo generated = disabledApp.service("TestPrivateAuthenticator");
        require(generated.enabled && generated.applicationInfo.enabled,
                "Fixture must exercise the generated enabled ApplicationInfo masking raw disabled owner state");
        require(disabledApp.accounts.getAuthenticatorTypes(0).length == 0,
                "Raw disabled authenticator owner registered through generated ApplicationInfo.enabled");
        System.out.println("MicrogAuthenticatorTest: disabled official/private components and raw disabled app owner are excluded while enabled Google login remains registered");

        File altered = new File(isolated, "altered-official.apk");
        Files.copy(new File(args[0]).toPath(), altered.toPath());
        try (RandomAccessFile bytes = new RandomAccessFile(altered, "rw")) {
            require(bytes.readInt() == 0x504b0304, "Official fixture is not a ZIP APK");
            // Change a signed ZIP local-header timestamp while keeping all descriptor resources parsable.
            bytes.seek(10); int before = bytes.readUnsignedByte(); bytes.seek(10); bytes.writeByte(before ^ 1);
        }
        Object tampered = parse(altered.getPath());
        require(!(Boolean) method(APK, "hasVerifiedMicrogSigner").invoke(tampered), "Modified APK retained the microG integrity pin");
        Registry modified = new Registry(tampered);
        require(!modified.service("GoogleLoginService").exported && modified.descriptor(GOOGLE) == null,
                "Private-service acceptance bypassed official Google signer/integrity verification");
        System.out.println("MicrogAuthenticatorTest: modified official APK retains parsable private metadata but cannot register Google authentication");
        System.out.println("MicrogAuthenticatorTest all passed");
    }
}
