/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
// Host-only declared Binder/Process/compatibility boundary; never packaged in Gesher.
package android.os;
public final class Process {
 public static int myUid(){return 10001;}
 public static UserHandle myUserHandle(){return new UserHandle(0);}
}
