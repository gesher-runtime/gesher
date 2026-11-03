// Gesher: com.gesher.app.shell.Shell's natives (gesher-app.jar), registered by
// android_runtime.cc: Start menu shortcuts, AppUserModelIDs and URL protocols
// for installed Android apps (shell track, docs/installer.md). The Win32 side is
// shell_win32.cc.

#include <jni.h>
#include <nativehelper/JNIHelp.h>

#include <string>

#include "core_jni_helpers.h"
#include "shell_win32.h"

namespace android {

int RegisterGesherNatives(JNIEnv* env, const char* className, const JNINativeMethod* methods,
                          int count);  // services_natives.cc

static std::u16string ShellString(JNIEnv* env, jstring s) {
    if (s == nullptr) return std::u16string();
    const jchar* chars = env->GetStringChars(s, nullptr);
    std::u16string result(reinterpret_cast<const char16_t*>(chars), env->GetStringLength(s));
    env->ReleaseStringChars(s, chars);
    return result;
}

static jstring ShellJavaString(JNIEnv* env, const std::u16string& s) {
    if (s.empty()) return nullptr;
    return env->NewString(reinterpret_cast<const jchar*>(s.data()), static_cast<jsize>(s.size()));
}

static jstring Shell_programsFolder(JNIEnv* env, jclass) {
    return ShellJavaString(env, gesher::shell::ProgramsFolder());
}

static jint Shell_createShortcut(JNIEnv* env, jclass, jstring link, jstring target,
                                 jstring arguments, jstring workDir, jstring icon,
                                 jstring description, jstring appId) {
    return static_cast<jint>(gesher::shell::CreateShortcut(
            ShellString(env, link), ShellString(env, target), ShellString(env, arguments),
            ShellString(env, workDir), ShellString(env, icon), ShellString(env, description),
            ShellString(env, appId)));
}

static jint Shell_setProcessAppId(JNIEnv* env, jclass, jstring appId) {
    return static_cast<jint>(gesher::shell::SetProcessAppId(ShellString(env, appId)));
}

static jstring Shell_processAppId(JNIEnv* env, jclass) {
    return ShellJavaString(env, gesher::shell::ProcessAppId());
}

static jboolean Shell_makeDirectories(JNIEnv* env, jclass, jstring dir) {
    return gesher::shell::MakeDirectories(ShellString(env, dir)) ? JNI_TRUE : JNI_FALSE;
}

static jboolean Shell_removeFile(JNIEnv* env, jclass, jstring path) {
    return gesher::shell::RemoveFile(ShellString(env, path)) ? JNI_TRUE : JNI_FALSE;
}

static jboolean Shell_removeEmptyDirectory(JNIEnv* env, jclass, jstring dir) {
    return gesher::shell::RemoveEmptyDirectory(ShellString(env, dir)) ? JNI_TRUE : JNI_FALSE;
}

static jboolean Shell_exists(JNIEnv* env, jclass, jstring path) {
    return gesher::shell::Exists(ShellString(env, path)) ? JNI_TRUE : JNI_FALSE;
}

static void Shell_notifyChanged(JNIEnv* env, jclass, jstring path) {
    gesher::shell::NotifyChanged(ShellString(env, path));
}

static jint Shell_registerUrlProtocol(JNIEnv* env, jclass, jstring scheme, jstring owner,
                                      jstring description, jstring command, jstring icon) {
    return static_cast<jint>(gesher::shell::RegisterUrlProtocol(
            ShellString(env, scheme), ShellString(env, owner), ShellString(env, description),
            ShellString(env, command), ShellString(env, icon)));
}

static jint Shell_unregisterUrlProtocol(JNIEnv* env, jclass, jstring scheme, jstring owner) {
    return static_cast<jint>(gesher::shell::UnregisterUrlProtocol(ShellString(env, scheme),
                                                                  ShellString(env, owner)));
}

static jstring Shell_urlProtocolOwner(JNIEnv* env, jclass, jstring scheme) {
    return ShellJavaString(env, gesher::shell::UrlProtocolOwner(ShellString(env, scheme)));
}

#define SHELL_STRING "Ljava/lang/String;"

static const JNINativeMethod gShellMethods[] = {
    {"nativeProgramsFolder", "()" SHELL_STRING, GESHER_JNI(Shell_programsFolder)},
    {"nativeCreateShortcut",
     "(" SHELL_STRING SHELL_STRING SHELL_STRING SHELL_STRING SHELL_STRING SHELL_STRING
     SHELL_STRING ")I",
     GESHER_JNI(Shell_createShortcut)},
    {"nativeSetProcessAppUserModelId", "(" SHELL_STRING ")I", GESHER_JNI(Shell_setProcessAppId)},
    {"nativeProcessAppUserModelId", "()" SHELL_STRING, GESHER_JNI(Shell_processAppId)},
    {"nativeMakeDirectories", "(" SHELL_STRING ")Z", GESHER_JNI(Shell_makeDirectories)},
    {"nativeRemoveFile", "(" SHELL_STRING ")Z", GESHER_JNI(Shell_removeFile)},
    {"nativeRemoveEmptyDirectory", "(" SHELL_STRING ")Z", GESHER_JNI(Shell_removeEmptyDirectory)},
    {"nativeExists", "(" SHELL_STRING ")Z", GESHER_JNI(Shell_exists)},
    {"nativeNotifyChanged", "(" SHELL_STRING ")V", GESHER_JNI(Shell_notifyChanged)},
    {"nativeRegisterUrlProtocol",
     "(" SHELL_STRING SHELL_STRING SHELL_STRING SHELL_STRING SHELL_STRING ")I",
     GESHER_JNI(Shell_registerUrlProtocol)},
    {"nativeUnregisterUrlProtocol", "(" SHELL_STRING SHELL_STRING ")I",
     GESHER_JNI(Shell_unregisterUrlProtocol)},
    {"nativeUrlProtocolOwner", "(" SHELL_STRING ")" SHELL_STRING,
     GESHER_JNI(Shell_urlProtocolOwner)},
};

int register_gesher_Shell(JNIEnv* env) {
    return RegisterGesherNatives(env, "com/gesher/app/shell/Shell", gShellMethods,
                                NELEM(gShellMethods));
}

}  // namespace android
