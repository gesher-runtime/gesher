package android.annotation;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

/** Compile-time stand-in (tests/dex-natives); only its name matters. */
@Retention(RetentionPolicy.CLASS)
public @interface IntDef { String[] prefix() default {}; long[] value() default {}; boolean flag() default false; }
