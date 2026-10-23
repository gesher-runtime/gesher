package com.android.internal.annotations;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

/** Compile-time stand-in (tests/dex-natives); only its name matters. */
@Retention(RetentionPolicy.CLASS)
public @interface VisibleForTesting { Visibility visibility() default Visibility.PRIVATE; enum Visibility { PROTECTED, PACKAGE, PRIVATE, NONE } }
