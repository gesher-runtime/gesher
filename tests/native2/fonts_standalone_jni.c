// A dedicated JNI entry avoids native2's framework/Bitmap registration path.
#include <jni.h>
#include <stddef.h>

int font_contract_main(int argc, char **argv);

static jint run(JNIEnv *env, jclass type, jobjectArray arguments) {
  (void)type;
  if ((*env)->GetArrayLength(env, arguments) != 6) return 2;
  jstring strings[6] = {0};
  const char *text[6] = {0};
  jint result = 2;
  for (int i = 0; i < 6; ++i) {
    strings[i] = (*env)->GetObjectArrayElement(env, arguments, i);
    if (!strings[i]) goto done;
    text[i] = (*env)->GetStringUTFChars(env, strings[i], NULL);
    if (!text[i]) goto done;
  }
  result = font_contract_main(6, (char **)text);
done:
  for (int i = 0; i < 6; ++i) {
    if (text[i]) (*env)->ReleaseStringUTFChars(env, strings[i], text[i]);
    if (strings[i]) (*env)->DeleteLocalRef(env, strings[i]);
  }
  return result;
}

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
  (void)reserved;
  JNIEnv *env = NULL;
  if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  jclass type = (*env)->FindClass(env, "gesher/Native3StandaloneFontTest");
  if (!type) return JNI_ERR;
  JNINativeMethod method = {"run", "([Ljava/lang/String;)I", (void *)&run};
  jint status = (*env)->RegisterNatives(env, type, &method, 1);
  (*env)->DeleteLocalRef(env, type);
  return status == JNI_OK ? JNI_VERSION_1_6 : JNI_ERR;
}
