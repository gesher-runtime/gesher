/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
// Host-only declared Binder/Process/compatibility boundary; never packaged in Gesher.
package android.os;
public class Binder {
 public static int uid=10001;
 public static int getCallingUid(){return uid;}
 public static long clearCallingIdentity(){long old=uid;uid=10001;return old;}
 public static void restoreCallingIdentity(long token){uid=(int)token;}
}
