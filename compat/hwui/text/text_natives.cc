// Gesher: the natives of gesher-app's text classes (rtl track, docs/fonts.md,
// docs/decisions/0044-rtl-scripts-and-locale.md):
//   com.gesher.app.fonts.WindowsFonts   where Windows keeps its fonts
//   com.gesher.app.locale.SystemLocale  the user's languages and regional format
// Registered from compat/hwui/graphics_runtime.cc when the classes are on the
// class path. JNICALL is System V under GESHER, as ART calls natives.

#include <jni.h>

#include <direct.h>

#include <cwctype>
#include <string>
#include <vector>

#include <nativehelper/JNIHelp.h>

#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#include "../../locale/preferred_languages.h"

namespace {

jstring ToJava(JNIEnv* env, const wchar_t* s, size_t n) {
  return env->NewString(reinterpret_cast<const jchar*>(s), static_cast<jsize>(n));
}

jstring ToJava(JNIEnv* env, const std::wstring& s) { return ToJava(env, s.data(), s.size()); }

// %WINDIR%\Fonts as a Gesher absolute path (ADR 0010, ADR 0045).
jstring JNICALL WindowsFonts_nativeFontsDirectory(JNIEnv* env, jclass) {
  wchar_t buf[MAX_PATH];
  UINT n = GetSystemWindowsDirectoryW(buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return nullptr;
  std::wstring dir(buf, n);
  if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
  dir += L"Fonts";
  for (wchar_t& c : dir) {
    if (c == L'\\') c = L'/';
  }
  int drive = _getdrive();  // 1 = A:, 0 when the current directory is a UNC path
  if (dir.size() >= 3 && dir[1] == L':' && dir[2] == L'/') {
    if (drive > 0 && towupper(dir[0]) == static_cast<wchar_t>(L'A' + drive - 1)) {
      dir.erase(0, 2);
    } else {
      dir = std::wstring(L"/mnt/") + static_cast<wchar_t>(towlower(dir[0])) + dir.substr(2);
    }
  }
  return ToJava(env, dir);
}

jobjectArray JNICALL SystemLocale_nativePreferredUiLanguages(JNIEnv* env, jclass) {
  std::vector<std::wstring> names;

  wchar_t* preferred = GesherPreferredLanguages();
  if (preferred != nullptr) {
    for (const wchar_t* name = preferred; *name != L'\0'; name += wcslen(name) + 1) {
      names.emplace_back(name);
    }
    HeapFree(GetProcessHeap(), 0, preferred);
  }

  if (names.empty()) {
    return nullptr;
  }
  jclass string_class = env->FindClass("java/lang/String");
  if (string_class == nullptr) return nullptr;
  jobjectArray out = env->NewObjectArray(static_cast<jsize>(names.size()), string_class, nullptr);
  if (out == nullptr) return nullptr;
  for (size_t i = 0; i < names.size(); ++i) {
    jstring s = ToJava(env, names[i]);
    if (s == nullptr) return nullptr;
    env->SetObjectArrayElement(out, static_cast<jsize>(i), s);
    env->DeleteLocalRef(s);
  }
  return out;
}

jstring JNICALL SystemLocale_nativeUserLocaleName(JNIEnv* env, jclass) {
  wchar_t buf[LOCALE_NAME_MAX_LENGTH];
  int n = GetUserDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH);
  if (n <= 1) return nullptr;
  return ToJava(env, buf, static_cast<size_t>(n - 1));
}

jstring JNICALL SystemLocale_nativeLocaleInfo(JNIEnv* env, jclass, jint type) {
  int n = GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, static_cast<LCTYPE>(type), nullptr, 0);
  if (n <= 0) return nullptr;
  std::vector<wchar_t> buf(static_cast<size_t>(n));
  n = GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, static_cast<LCTYPE>(type), buf.data(), n);
  if (n <= 0) return nullptr;
  return ToJava(env, buf.data(), static_cast<size_t>(n - 1));
}

const JNINativeMethod kWindowsFontsMethods[] = {
    {"nativeFontsDirectory", "()Ljava/lang/String;",
     reinterpret_cast<void*>(&WindowsFonts_nativeFontsDirectory)},
};

const JNINativeMethod kSystemLocaleMethods[] = {
    {"nativePreferredUiLanguages", "()[Ljava/lang/String;",
     reinterpret_cast<void*>(&SystemLocale_nativePreferredUiLanguages)},
    {"nativeUserLocaleName", "()Ljava/lang/String;",
     reinterpret_cast<void*>(&SystemLocale_nativeUserLocaleName)},
    {"nativeLocaleInfo", "(I)Ljava/lang/String;",
     reinterpret_cast<void*>(&SystemLocale_nativeLocaleInfo)},
};

}  // namespace

int register_com_gesher_app_fonts_WindowsFonts(JNIEnv* env) {
  return jniRegisterNativeMethods(env, "com/gesher/app/fonts/WindowsFonts", kWindowsFontsMethods,
                                  sizeof(kWindowsFontsMethods) / sizeof(kWindowsFontsMethods[0]));
}

int register_com_gesher_app_locale_SystemLocale(JNIEnv* env) {
  return jniRegisterNativeMethods(env, "com/gesher/app/locale/SystemLocale", kSystemLocaleMethods,
                                  sizeof(kSystemLocaleMethods) / sizeof(kSystemLocaleMethods[0]));
}
