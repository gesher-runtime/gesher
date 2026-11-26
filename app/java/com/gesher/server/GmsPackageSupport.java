/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.pm.Signature;

/**
 * Package names of Google Play services and the Play Store, and Google's public app
 * signing certificate for an installed, verified microG (GmsSignatureSpoofing).
 *
 * <p>Neither package is reported unless it is installed: without microG, Play services
 * and the Play Store are unknown packages, as on an Android device without them, and
 * client libraries report {@code SERVICE_MISSING} (issue #335).
 */
public final class GmsPackageSupport {
    public static final String PACKAGE_GMS = "com.google.android.gms";
    public static final String PACKAGE_VENDING = "com.android.vending";

    // Google's app signing certificate (SHA-1 38:91:8A:45:3D:07:19:93:54:F8:B1:9A:F0:5E:C6:56:2C:ED:57:88),
    // public in every APK Google signs with it (Play services, Gmail...). The client
    // libraries' GoogleSignatureVerifier accepts Play services only with one of Google's
    // certificates. Only the certificate; no key. GmsSignatureSpoofing reports it for an
    // installed microG whose own signature it has verified.
    public static final String GOOGLE_CERT_HEX =
            "308204433082032ba003020102020900c2e08746644a308d300d06092a864886f70d010104050030" +
            "74310b3009060355040613025553311330110603550408130a43616c69666f726e69613116301406" +
            "03550407130d4d6f756e7461696e205669657731143012060355040a130b476f6f676c6520496e63" +
            "2e3110300e060355040b1307416e64726f69643110300e06035504031307416e64726f6964301e17" +
            "0d3038303832313233313333345a170d3336303130373233313333345a3074310b30090603550406" +
            "13025553311330110603550408130a43616c69666f726e6961311630140603550407130d4d6f756e" +
            "7461696e205669657731143012060355040a130b476f6f676c6520496e632e3110300e060355040b" +
            "1307416e64726f69643110300e06035504031307416e64726f696430820120300d06092a864886f7" +
            "0d01010105000382010d00308201080282010100ab562e00d83ba208ae0a966f124e29da11f2ab56" +
            "d08f58e2cca91303e9b754d372f640a71b1dcb130967624e4656a7776a92193db2e5bfb724a91e77" +
            "188b0e6a47a43b33d9609b77183145ccdf7b2e586674c9e1565b1f4c6a5955bff251a63dabf9c55c" +
            "27222252e875e4f8154a645f897168c0b1bfc612eabf785769bb34aa7984dc7e2ea2764cae8307d8" +
            "c17154d7ee5f64a51a44a602c249054157dc02cd5f5c0e55fbef8519fbe327f0b1511692c5a06f19" +
            "d18385f5c4dbc2d6b93f68cc2979c70e18ab93866b3bd5db8999552a0e3b4c99df58fb918bedc182" +
            "ba35e003c1b4b10dd244a8ee24fffd333872ab5221985edab0fc0d0b145b6aa192858e79020103a3" +
            "81d93081d6301d0603551d0e04160414c77d8cc2211756259a7fd382df6be398e4d786a53081a606" +
            "03551d2304819e30819b8014c77d8cc2211756259a7fd382df6be398e4d786a5a178a4763074310b" +
            "3009060355040613025553311330110603550408130a43616c69666f726e69613116301406035504" +
            "07130d4d6f756e7461696e205669657731143012060355040a130b476f6f676c6520496e632e3110" +
            "300e060355040b1307416e64726f69643110300e06035504031307416e64726f6964820900c2e087" +
            "46644a308d300c0603551d13040530030101ff300d06092a864886f70d010104050003820101006d" +
            "d252ceef85302c360aaace939bcff2cca904bb5d7a1661f8ae46b2994204d0ff4a68c7ed1a531ec4" +
            "595a623ce60763b167297a7ae35712c407f208f0cb109429124d7b106219c084ca3eb3f9ad5fb871" +
            "ef92269a8be28bf16d44c8d9a08e6cb2f005bb3fe2cb96447e868e731076ad45b33f6009ea19c161" +
            "e62641aa99271dfd5228c5c587875ddb7f452758d661f6cc0cccb7352e424cc4365c523532f73251" +
            "37593c4ae341f4db41edda0d0b1071a7c440f0fe9ea01cb627ca674369d084bd2fd911ff06cdbf2c" +
            "fa10dc0f893ae35762919048c7efc64c7144178342f70581c9de573af55b390dd7fdb9418631895d" +
            "5f759f30112687ff621410c069308a";

    private static volatile Signature sGoogleSignature;

    private GmsPackageSupport() {
    }

    public static Signature getGoogleSignature() {
        if (sGoogleSignature == null) {
            sGoogleSignature = new Signature(GOOGLE_CERT_HEX);
        }
        return sGoogleSignature;
    }
}
