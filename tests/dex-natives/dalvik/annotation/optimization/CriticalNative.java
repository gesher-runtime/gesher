package dalvik.annotation.optimization;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

/**
 * Compile-time copy of libcore's annotation (the tests build without the boot
 * class path on the compile class path). ART matches the annotation by
 * descriptor in the dex metadata, so this copy marks natives the same way.
 */
@Retention(RetentionPolicy.CLASS)
@Target(ElementType.METHOD)
public @interface CriticalNative {}
