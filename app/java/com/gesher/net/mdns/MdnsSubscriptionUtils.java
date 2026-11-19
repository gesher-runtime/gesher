/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.gesher.net.mdns;

import static com.android.net.module.util.DnsUtils.equalsDnsLabelIgnoreDnsCase;
import static com.android.net.module.util.DnsUtils.equalsIgnoreDnsCase;

import android.annotation.NonNull;
import android.os.Build;
import android.os.SystemClock;
import android.util.ArraySet;
import java.util.HashSet;
import java.util.Set;

/** Complete selected upstream utility declarations for the response/query core. */
public final class MdnsSubscriptionUtils {
    private MdnsSubscriptionUtils() {}

    public static boolean typeEqualsOrIsSubtype(@NonNull String[] a,
            @NonNull String[] b) {
        return equalsDnsLabelIgnoreDnsCase(a, b)
                || ((b.length == (a.length + 2))
                && equalsIgnoreDnsCase(b[1], MdnsConstants.SUBTYPE_LABEL)
                && MdnsRecord.labelsAreSuffix(a, b));
    }

    public static <Type> Set<Type> newSet() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            return new ArraySet<>();
        } else {
            return new HashSet<>();
        }
    }

    public static class Clock {
        /**
         * @see SystemClock#elapsedRealtime
         */
        public long elapsedRealtime() {
            return SystemClock.elapsedRealtime();
        }
    }
}
