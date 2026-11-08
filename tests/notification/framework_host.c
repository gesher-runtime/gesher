/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Host diagnostic boundaries only. The service, channel/group values, manager caches,
 * framework XML serializers and AtomicFile are the real compiled Java classes.
 * This models a local system caller, JDK arrays, monotonic uptime and the
 * host filesystem; it does not implement or test ART, Binder transactions or UI.
 * Use text XML on this JVM, whose libcore fast binary-XML natives are unavailable.
 */
#include <jni.h>
JNIEXPORT jboolean JNICALL Java_android_util_Log_isLoggable(JNIEnv* env,jclass cls,jstring tag,jint priority) { return JNI_FALSE; }
JNIEXPORT jint JNICALL Java_android_util_Log_println_1native(JNIEnv* env,jclass cls,jint buf,jint priority,jstring tag,jstring msg) { return 0; }
JNIEXPORT jint JNICALL Java_libcore_io_Linux_nativeGetuid(JNIEnv* env,jclass cls) { return 10000; }
JNIEXPORT jobject JNICALL Java_dalvik_system_VMRuntime_newUnpaddedArray(JNIEnv* env,jobject runtime,jclass component,jint length) {
  jclass array=(*env)->FindClass(env,"java/lang/reflect/Array");
  jmethodID create=(*env)->GetStaticMethodID(env,array,"newInstance","(Ljava/lang/Class;I)Ljava/lang/Object;");
  return (*env)->CallStaticObjectMethod(env,array,create,component,length);
}
#include <time.h>
JNIEXPORT jlong JNICALL Java_android_os_SystemClock_uptimeMillis(JNIEnv* env,jclass cls) {
 struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now); return (jlong)now.tv_sec*1000+now.tv_nsec/1000000;
}
#include <string.h>
JNIEXPORT jstring JNICALL Java_android_os_SystemProperties_native_1get__Ljava_lang_String_2Ljava_lang_String_2(JNIEnv* env,jclass cls,jstring key,jstring fallback) {
 const char* name=(*env)->GetStringUTFChars(env,key,0); const char* value=0;
 if (!strcmp(name,"ro.product.cpu.abilist") || !strcmp(name,"ro.product.cpu.abilist64")) value="x86_64";
 else if (!strcmp(name,"ro.product.cpu.abilist32")) value="x86";
 else if (!strcmp(name,"ro.build.version.codename") || !strcmp(name,"ro.build.version.all_codenames")) value="REL";
 (*env)->ReleaseStringUTFChars(env,key,name);
 return value ? (*env)->NewStringUTF(env,value) : fallback;
}
JNIEXPORT jint JNICALL Java_android_os_SystemProperties_native_1get_1int__Ljava_lang_String_2I(JNIEnv* env,jclass cls,jstring key,jint fallback) {
 const char* name=(*env)->GetStringUTFChars(env,key,0); jint value=!strcmp(name,"ro.build.version.sdk")?36:fallback;(*env)->ReleaseStringUTFChars(env,key,name);return value;
}
JNIEXPORT jlong JNICALL Java_android_os_SystemProperties_native_1get_1long__Ljava_lang_String_2J(JNIEnv* env,jclass cls,jstring key,jlong fallback) {return fallback;}
JNIEXPORT jboolean JNICALL Java_android_os_SystemProperties_native_1get_1boolean__Ljava_lang_String_2Z(JNIEnv* env,jclass cls,jstring key,jboolean fallback) { const char* name=(*env)->GetStringUTFChars(env,key,0); jboolean result=!strcmp(name,"persist.sys.binary_xml")?JNI_FALSE:fallback; (*env)->ReleaseStringUTFChars(env,key,name); return result; }
JNIEXPORT jlong JNICALL Java_android_os_SystemProperties_native_1find(JNIEnv* env,jclass cls,jstring key) {return 0;}
JNIEXPORT jboolean JNICALL Java_dalvik_system_VMRuntime_is64Bit(JNIEnv* env,jobject runtime) {return JNI_TRUE;}

#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
static void throw_errno(JNIEnv* env, const char* operation, int error) {
 jclass type=(*env)->FindClass(env,"android/system/ErrnoException");
 jmethodID constructor=(*env)->GetMethodID(env,type,"<init>","(Ljava/lang/String;I)V");
 jstring name=(*env)->NewStringUTF(env,operation);
 jthrowable exception=(jthrowable)(*env)->NewObject(env,type,constructor,name,error);
 (*env)->Throw(env,exception);
}
JNIEXPORT jint JNICALL Java_android_os_Binder_getCallingPid(JNIEnv* env,jclass cls) { return getpid(); }
JNIEXPORT jint JNICALL Java_android_os_Binder_getCallingUid(JNIEnv* env,jclass cls) { return 1000; }
JNIEXPORT jlong JNICALL Java_android_os_Binder_clearCallingIdentity(JNIEnv* env,jclass cls) { return 0; }
JNIEXPORT void JNICALL Java_android_os_Binder_restoreCallingIdentity(JNIEnv* env,jclass cls,jlong identity) { }
JNIEXPORT jint JNICALL Java_libcore_io_Linux_nativeGetpid(JNIEnv* env,jclass cls) { return getpid(); }
JNIEXPORT void JNICALL Java_libcore_io_Linux_chmod(JNIEnv* env,jobject self,jstring path,jint mode) {
 const char* p=(*env)->GetStringUTFChars(env,path,0); if (!p) return;
 int result=chmod(p,mode), error=errno; (*env)->ReleaseStringUTFChars(env,path,p);
 if (result<0) throw_errno(env,"chmod",error);
}
JNIEXPORT jint JNICALL Java_libcore_io_Linux_preadBytes(JNIEnv* env,jobject self,jobject fd,jobject bytes,jint offset,jint count,jlong position) {
 if (!(*env)->IsInstanceOf(env,bytes,(*env)->FindClass(env,"[B"))) {
   (*env)->ThrowNew(env,(*env)->FindClass(env,"java/lang/UnsupportedOperationException"),"Host fixture only reads byte arrays"); return -1;
 }
 jsize length=(*env)->GetArrayLength(env,(jbyteArray)bytes);
 if (offset<0 || count<0 || offset>length || count>length-offset) {
   (*env)->ThrowNew(env,(*env)->FindClass(env,"java/lang/IndexOutOfBoundsException"),"Invalid pread range"); return -1;
 }
 jclass type=(*env)->GetObjectClass(env,fd); jfieldID field=(*env)->GetFieldID(env,type,"fd","I"); int raw=(*env)->GetIntField(env,fd,field);
 jbyte* buffer=(*env)->GetByteArrayElements(env,(jbyteArray)bytes,0); if (!buffer) return -1;
 ssize_t result=pread(raw,buffer+offset,count,position); int error=errno;
 (*env)->ReleaseByteArrayElements(env,(jbyteArray)bytes,buffer,0);
 if (result<0) throw_errno(env,"pread",error);
 return result;
}
