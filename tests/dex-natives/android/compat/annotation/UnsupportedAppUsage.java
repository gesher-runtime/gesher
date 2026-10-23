package android.compat.annotation;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

/** Compile-time stand-in (tests/dex-natives); only its name matters. */
@Retention(RetentionPolicy.CLASS)
public @interface UnsupportedAppUsage { int maxTargetSdk() default Integer.MAX_VALUE; long trackingBug() default 0; String implicitMember() default ""; String expectedSignature() default ""; String publicAlternatives() default ""; }
