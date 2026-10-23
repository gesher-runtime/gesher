package android.annotation;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

/** Compile-time stand-in (tests/dex-natives); only its name matters. */
@Retention(RetentionPolicy.CLASS)
public @interface Nullable {}
