package android.annotation;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

/** Compile-time stand-in (tests/dex-natives); only its name matters. */
@Retention(RetentionPolicy.CLASS)
public @interface SystemApi {
  Client client() default Client.PRIVILEGED_APPS;

  enum Client { PRIVILEGED_APPS, MODULE_LIBRARIES, SYSTEM_SERVER }
}
