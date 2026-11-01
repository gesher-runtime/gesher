#include <jni.h>
#include <stddef.h>
int font_environment_probe(int,char **,char **);
static jint run(JNIEnv *env,jclass type,jobjectArray arguments){
  (void)type;jsize count=(*env)->GetArrayLength(env,arguments);if(count!=9)return 2;
  jstring strings[9]={0};const char *values[9]={0};int result=2;
  for(int i=0;i<count;i++){strings[i]=(*env)->GetObjectArrayElement(env,arguments,i);if(!strings[i])goto done;values[i]=(*env)->GetStringUTFChars(env,strings[i],NULL);if(!values[i])goto done;}
  result=font_environment_probe(count,(char**)values,NULL);
done:
  for(int i=0;i<count;i++){if(values[i])(*env)->ReleaseStringUTFChars(env,strings[i],values[i]);if(strings[i])(*env)->DeleteLocalRef(env,strings[i]);}
  return result;
}
JNIEXPORT jint JNI_OnLoad(JavaVM *vm,void *reserved){
  (void)reserved;JNIEnv *env=NULL;if((*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6)!=JNI_OK)return JNI_ERR;
  jclass type=(*env)->FindClass(env,"gesher/Native3FontEnvironmentTest");if(!type)return JNI_ERR;
  JNINativeMethod method={"run","([Ljava/lang/String;)I",(void*)&run};jint result=(*env)->RegisterNatives(env,type,&method,1);(*env)->DeleteLocalRef(env,type);
  return result==JNI_OK?JNI_VERSION_1_6:JNI_ERR;
}
