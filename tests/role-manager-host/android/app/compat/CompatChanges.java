/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
// Host-only declared Binder/Process/compatibility boundary; never packaged in Gesher.
package android.app.compat;
public final class CompatChanges{
 public static boolean modern=true;
 public static boolean isChangeEnabled(long id){if(id!=303742236L)throw new AssertionError(id);return modern;}
}
