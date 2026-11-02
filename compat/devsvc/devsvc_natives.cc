// Gesher: com.gesher.server.WindowsHost's natives (gesher-app.jar), registered
// by android_runtime.cc: what the system server's device-state services ask of
// Windows (devsvc track, docs/services.md "Device services"). The Win32 side
// is devsvc_win32.cc and devsvc_toast.c.

#include <jni.h>
#include <nativehelper/JNIHelp.h>

#include <errno.h>
#include <fcntl.h>
#include <string>

#include "core_jni_helpers.h"
#include "devsvc_win32.h"

extern "C" void __stdcall Sleep(unsigned long dwMilliseconds);

namespace android {

int RegisterGesherNatives(JNIEnv* env, const char* className, const JNINativeMethod* methods,
                          int count);  // services_natives.cc

namespace {

int GetFd(JNIEnv* env, jobject fdo) {
  if (fdo == nullptr) return -1;
  jclass cls = env->GetObjectClass(fdo);
  if (cls == nullptr) return -1;
  jfieldID fid = env->GetFieldID(cls, "descriptor", "I");
  env->DeleteLocalRef(cls);
  if (fid == nullptr) {
    env->ExceptionClear();
    return -1;
  }
  return env->GetIntField(fdo, fid);
}

void ThrowIoException(JNIEnv* env, const char* msg) {
  jclass ioe = env->FindClass("java/io/IOException");
  if (ioe != nullptr) {
    env->ThrowNew(ioe, msg);
    env->DeleteLocalRef(ioe);
  }
}

void ThrowNullPointerException(JNIEnv* env, const char* msg) {
  jclass npe = env->FindClass("java/lang/NullPointerException");
  if (npe != nullptr) {
    env->ThrowNew(npe, msg);
    env->DeleteLocalRef(npe);
  }
}

jint Gesher_FileDispatcherImpl_lock0(JNIEnv* env, jclass, jobject fdo, jboolean block,
                                     jlong pos, jlong size, jboolean shared) {
  if (fdo == nullptr) {
    ThrowNullPointerException(env, "FileDescriptor is null");
    return -1;
  }
  int fd = GetFd(env, fdo);
  if (fd < 0) {
    ThrowIoException(env, "Bad file descriptor");
    return -1;
  }

  struct flock fl{};
  fl.l_whence = SEEK_SET;
  fl.l_start = static_cast<off_t>(pos);
  fl.l_len = (size == 0x7fffffffffffffffLL) ? 0 : static_cast<off_t>(size);
  fl.l_type = (shared == JNI_TRUE) ? F_RDLCK : F_WRLCK;

  int ret = -1;
  const int max_attempts = (block == JNI_TRUE) ? 200 : 1;
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    ret = fcntl(fd, F_SETLK, &fl);
    if (ret == 0) {
      return 0;  // sun.nio.ch.FileDispatcher.LOCKED
    }
    if (errno != EAGAIN && errno != EACCES) {
      break;
    }
    if (block == JNI_TRUE) {
      Sleep(10);
    }
  }

  if (ret != 0) {
    if (errno == EAGAIN || errno == EACCES) {
      return -1;  // sun.nio.ch.FileDispatcher.NO_LOCK
    }
    if (errno == EINTR) {
      return 2;  // sun.nio.ch.FileDispatcher.INTERRUPTED
    }
    ThrowIoException(env, "Lock failed");
    return -1;
  }
  return 0;
}

void Gesher_FileDispatcherImpl_release0(JNIEnv* env, jclass, jobject fdo, jlong pos,
                                        jlong size) {
  if (fdo == nullptr) {
    ThrowNullPointerException(env, "FileDescriptor is null");
    return;
  }
  int fd = GetFd(env, fdo);
  if (fd < 0) {
    ThrowIoException(env, "Bad file descriptor");
    return;
  }

  struct flock fl{};
  fl.l_whence = SEEK_SET;
  fl.l_start = static_cast<off_t>(pos);
  fl.l_len = (size == 0x7fffffffffffffffLL) ? 0 : static_cast<off_t>(size);
  fl.l_type = F_UNLCK;

  if (fcntl(fd, F_SETLK, &fl) < 0) {
    ThrowIoException(env, "Release failed");
  }
}

const JNINativeMethod gFileDispatcherMethods[] = {
    {"lock0", "(Ljava/io/FileDescriptor;ZJJZ)I", GESHER_JNI(Gesher_FileDispatcherImpl_lock0)},
    {"release0", "(Ljava/io/FileDescriptor;JJ)V", GESHER_JNI(Gesher_FileDispatcherImpl_release0)},
};

namespace dev = gesher::devsvc;

std::u16string Str(JNIEnv* env, jstring s) {
  if (s == nullptr) return std::u16string();
  const jchar* chars = env->GetStringChars(s, nullptr);
  std::u16string result(reinterpret_cast<const char16_t*>(chars), env->GetStringLength(s));
  env->ReleaseStringChars(s, chars);
  return result;
}

jstring JStr(JNIEnv* env, const std::u16string& s) {
  return env->NewString(reinterpret_cast<const jchar*>(s.data()), static_cast<jsize>(s.size()));
}

jlong WindowsHost_startEvents(JNIEnv*, jclass) {
  return static_cast<jlong>(dev::StartEvents());
}

jint WindowsHost_waitEvents(JNIEnv*, jclass, jint timeoutMs) {
  return static_cast<jint>(dev::WaitEvents(timeoutMs));
}

jobjectArray WindowsHost_nextActivation(JNIEnv* env, jclass) {
  std::u16string app_id, tag, group, arguments;
  if (!dev::PopActivation(&app_id, &tag, &group, &arguments)) return nullptr;
  jclass string_class = env->FindClass("java/lang/String");
  jobjectArray out = env->NewObjectArray(4, string_class, nullptr);
  env->DeleteLocalRef(string_class);
  if (out == nullptr) return nullptr;
  const std::u16string* parts[] = {&app_id, &tag, &group, &arguments};
  for (jsize i = 0; i < 4; i++) {
    jstring s = JStr(env, *parts[i]);
    env->SetObjectArrayElement(out, i, s);
    env->DeleteLocalRef(s);
  }
  return out;
}

jstring WindowsHost_clipboardText(JNIEnv* env, jclass) {
  std::u16string text;
  return dev::ClipboardText(&text) ? JStr(env, text) : nullptr;
}

jstring WindowsHost_clipboardHtml(JNIEnv* env, jclass) {
  std::u16string html;
  return dev::ClipboardHtml(&html) ? JStr(env, html) : nullptr;
}

jboolean WindowsHost_setClipboard(JNIEnv* env, jclass, jstring text, jstring html) {
  return dev::SetClipboard(Str(env, text), Str(env, html)) ? JNI_TRUE : JNI_FALSE;
}

jint WindowsHost_clipboardSequence(JNIEnv*, jclass) {
  return static_cast<jint>(dev::ClipboardSequence());
}

jint WindowsHost_alarmWait(JNIEnv*, jclass, jlong utcMillis, jboolean wakeup, jint timeoutMs) {
  return dev::AlarmWait(utcMillis, wakeup == JNI_TRUE, timeoutMs);
}

void WindowsHost_alarmKick(JNIEnv*, jclass) { dev::AlarmKick(); }

jintArray WindowsHost_powerStatus(JNIEnv* env, jclass) {
  int status[4];
  if (!dev::PowerStatus(status)) return nullptr;
  jintArray out = env->NewIntArray(4);
  if (out == nullptr) return nullptr;
  jint values[4] = {status[0], status[1], status[2], status[3]};
  env->SetIntArrayRegion(out, 0, 4, values);
  return out;
}

jboolean WindowsHost_keepAwake(JNIEnv* env, jclass, jstring reason, jboolean system,
                               jboolean display) {
  return dev::KeepAwake(Str(env, reason), system == JNI_TRUE, display == JNI_TRUE) ? JNI_TRUE
                                                                                     : JNI_FALSE;
}

jlong WindowsHost_idleMillis(JNIEnv*, jclass) { return static_cast<jlong>(dev::IdleMillis()); }

jint WindowsHost_networkState(JNIEnv*, jclass) { return dev::NetworkState(); }

jstring WindowsHost_timeZoneKeyName(JNIEnv* env, jclass) {
  std::u16string name = dev::TimeZoneKeyName();
  return name.empty() ? nullptr : JStr(env, name);
}

jstring WindowsHost_ianaZone(JNIEnv* env, jclass, jstring windowsId) {
  std::u16string id = dev::IanaZone(Str(env, windowsId));
  return id.empty() ? nullptr : JStr(env, id);
}

jint WindowsHost_showToast(JNIEnv* env, jclass, jstring appId, jstring tag, jstring group,
                           jstring xml, jboolean suppressPopup, jboolean high) {
  return static_cast<jint>(dev::ShowToast(Str(env, appId), Str(env, tag), Str(env, group),
                                          Str(env, xml), suppressPopup == JNI_TRUE,
                                          high == JNI_TRUE));
}

jint WindowsHost_removeToast(JNIEnv* env, jclass, jstring appId, jstring tag, jstring group) {
  return static_cast<jint>(dev::RemoveToast(Str(env, appId), Str(env, tag), Str(env, group)));
}

jint WindowsHost_clearToasts(JNIEnv* env, jclass, jstring appId) {
  return static_cast<jint>(dev::ClearToasts(Str(env, appId)));
}

jint WindowsHost_registerAppId(JNIEnv* env, jclass, jstring appId, jstring displayName,
                               jstring icon) {
  return static_cast<jint>(
      dev::RegisterAppId(Str(env, appId), Str(env, displayName), Str(env, icon)));
}

jint WindowsHost_unregisterAppId(JNIEnv* env, jclass, jstring appId) {
  return static_cast<jint>(dev::UnregisterAppId(Str(env, appId)));
}

jint WindowsHost_registerActivator(JNIEnv*, jclass, jboolean singleUse) {
  return static_cast<jint>(dev::RegisterActivator(singleUse == JNI_TRUE));
}

void WindowsHost_revokeActivator(JNIEnv*, jclass) { dev::RevokeActivator(); }

jstring WindowsHost_fullPath(JNIEnv* env, jclass, jstring path) {
  return JStr(env, dev::FullPath(Str(env, path)));
}

jboolean WindowsHost_openUrl(JNIEnv* env, jclass, jstring url) {
  return dev::OpenUrl(Str(env, url)) ? JNI_TRUE : JNI_FALSE;
}

extern "C" {
jbyteArray Java_com_gesher_server_WindowsHost_nativeClipboardImage(JNIEnv* env, jclass);
jboolean Java_com_gesher_server_WindowsHost_nativeHasClipboardImage(JNIEnv*, jclass);
jboolean Java_com_gesher_server_WindowsHost_nativeSetClipboardRich(JNIEnv* env, jclass,
                                                                   jstring, jstring,
                                                                   jbyteArray);
jint Java_com_gesher_server_WindowsHost_nativeShowNotificationBridge(
    JNIEnv* env, jclass, jstring, jstring, jstring, jstring, jstring, jstring, jint);
jint Java_com_gesher_server_WindowsHost_nativeCancelNotificationBridge(
    JNIEnv* env, jclass, jstring, jstring, jstring, jint);
}

#define DEVSVC_STRING "Ljava/lang/String;"

const JNINativeMethod gWindowsHostMethods[] = {
    {"nativeStartEvents", "()J", GESHER_JNI(WindowsHost_startEvents)},
    {"nativeWaitEvents", "(I)I", GESHER_JNI(WindowsHost_waitEvents)},
    {"nativeNextActivation", "()[" DEVSVC_STRING, GESHER_JNI(WindowsHost_nextActivation)},
    {"nativeClipboardText", "()" DEVSVC_STRING, GESHER_JNI(WindowsHost_clipboardText)},
    {"nativeClipboardHtml", "()" DEVSVC_STRING, GESHER_JNI(WindowsHost_clipboardHtml)},
    {"nativeClipboardImage", "()[B",
     GESHER_JNI(Java_com_gesher_server_WindowsHost_nativeClipboardImage)},
    {"nativeHasClipboardImage", "()Z",
     GESHER_JNI(Java_com_gesher_server_WindowsHost_nativeHasClipboardImage)},
    {"nativeSetClipboard", "(" DEVSVC_STRING DEVSVC_STRING ")Z",
     GESHER_JNI(WindowsHost_setClipboard)},
    {"nativeSetClipboardRich", "(" DEVSVC_STRING DEVSVC_STRING "[B)Z",
     GESHER_JNI(Java_com_gesher_server_WindowsHost_nativeSetClipboardRich)},
    {"nativeClipboardSequence", "()I", GESHER_JNI(WindowsHost_clipboardSequence)},
    {"nativeAlarmWait", "(JZI)I", GESHER_JNI(WindowsHost_alarmWait)},
    {"nativeAlarmKick", "()V", GESHER_JNI(WindowsHost_alarmKick)},
    {"nativePowerStatus", "()[I", GESHER_JNI(WindowsHost_powerStatus)},
    {"nativeKeepAwake", "(" DEVSVC_STRING "ZZ)Z", GESHER_JNI(WindowsHost_keepAwake)},
    {"nativeIdleMillis", "()J", GESHER_JNI(WindowsHost_idleMillis)},
    {"nativeNetworkState", "()I", GESHER_JNI(WindowsHost_networkState)},
    {"nativeTimeZoneKeyName", "()" DEVSVC_STRING, GESHER_JNI(WindowsHost_timeZoneKeyName)},
    {"nativeShowToast", "(" DEVSVC_STRING DEVSVC_STRING DEVSVC_STRING DEVSVC_STRING "ZZ)I",
     GESHER_JNI(WindowsHost_showToast)},
    {"nativeRemoveToast", "(" DEVSVC_STRING DEVSVC_STRING DEVSVC_STRING ")I",
     GESHER_JNI(WindowsHost_removeToast)},
    {"nativeClearToasts", "(" DEVSVC_STRING ")I", GESHER_JNI(WindowsHost_clearToasts)},
    {"nativeShowNotificationBridge",
     "(" DEVSVC_STRING DEVSVC_STRING DEVSVC_STRING DEVSVC_STRING DEVSVC_STRING DEVSVC_STRING "I)I",
     GESHER_JNI(Java_com_gesher_server_WindowsHost_nativeShowNotificationBridge)},
    {"nativeCancelNotificationBridge", "(" DEVSVC_STRING DEVSVC_STRING DEVSVC_STRING "I)I",
     GESHER_JNI(Java_com_gesher_server_WindowsHost_nativeCancelNotificationBridge)},
    {"nativeRegisterAppId", "(" DEVSVC_STRING DEVSVC_STRING DEVSVC_STRING ")I",
     GESHER_JNI(WindowsHost_registerAppId)},
    {"nativeUnregisterAppId", "(" DEVSVC_STRING ")I", GESHER_JNI(WindowsHost_unregisterAppId)},
    {"nativeFullPath", "(" DEVSVC_STRING ")" DEVSVC_STRING, GESHER_JNI(WindowsHost_fullPath)},
    {"nativeOpenUrl", "(" DEVSVC_STRING ")Z", GESHER_JNI(WindowsHost_openUrl)},
    {"nativeIanaZone", "(" DEVSVC_STRING ")" DEVSVC_STRING, GESHER_JNI(WindowsHost_ianaZone)},
    {"nativeRegisterActivator", "(Z)I", GESHER_JNI(WindowsHost_registerActivator)},
    {"nativeRevokeActivator", "()V", GESHER_JNI(WindowsHost_revokeActivator)},
};

}  // namespace

int register_gesher_WindowsHost(JNIEnv* env) {
  RegisterGesherNatives(env, "sun/nio/ch/FileDispatcherImpl", gFileDispatcherMethods,
                        NELEM(gFileDispatcherMethods));
  return RegisterGesherNatives(env, "com/gesher/server/WindowsHost", gWindowsHostMethods,
                               NELEM(gWindowsHostMethods));
}

}  // namespace android
