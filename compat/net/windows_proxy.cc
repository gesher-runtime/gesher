// Gesher: Windows' proxy settings for Android's (docs/network.md, ADR 0058):
// com.gesher.net.WindowsProxy's natives, the builtin library
// libgesher_proxy.so.
//
// The user's settings (Internet Options: a proxy server and its bypass list,
// a PAC script's URL, automatic detection) come from WinHTTP's
// WinHttpGetIEProxyConfigForCurrentUser; the proxy for a URL under a PAC
// script or WPAD is WinHttpGetProxyForUrl's (Windows runs the script); a
// change of the settings wakes RegNotifyChangeKeyValue on their key.

#include <windows.h>
#include <winhttp.h>

#include <stdio.h>

#include <string>

#include <gesher/builtin_library.h>
#include <jni.h>
#include <nativehelper/jni_macros.h>

namespace {

std::string Utf8(const wchar_t* s) {
  if (s == nullptr || *s == 0) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  std::string out(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, nullptr, nullptr);
  return out;
}

std::wstring Wide(JNIEnv* env, jstring s) {
  if (s == nullptr) return std::wstring();
  jsize n = env->GetStringLength(s);
  std::wstring out(static_cast<size_t>(n), L'\0');
  if (n > 0) env->GetStringRegion(s, 0, n, reinterpret_cast<jchar*>(&out[0]));
  return out;
}

// "auto\t1\npac\t<url>\nproxy\t<server>\nbypass\t<list>\n" (empty fields when unset).
jstring WindowsProxy_nativeConfig(JNIEnv* env, jclass) {
  WINHTTP_CURRENT_USER_IE_PROXY_CONFIG c = {};
  std::string out;
  if (WinHttpGetIEProxyConfigForCurrentUser(&c)) {
    out += "auto\t";
    out += c.fAutoDetect ? "1" : "0";
    out += "\npac\t" + Utf8(c.lpszAutoConfigUrl);
    out += "\nproxy\t" + Utf8(c.lpszProxy);
    out += "\nbypass\t" + Utf8(c.lpszProxyBypass);
    out += "\n";
    if (c.lpszAutoConfigUrl != nullptr) GlobalFree(c.lpszAutoConfigUrl);
    if (c.lpszProxy != nullptr) GlobalFree(c.lpszProxy);
    if (c.lpszProxyBypass != nullptr) GlobalFree(c.lpszProxyBypass);
  } else {
    char buf[64];
    snprintf(buf, sizeof buf, "error\t%lu\n", GetLastError());
    out = buf;
  }
  return env->NewStringUTF(out.c_str());
}

// What a PAC script (or WPAD when pac is empty) says for url, as a PAC result:
// "DIRECT" or "PROXY host:port; PROXY ..."; null when it cannot be had.
jstring WindowsProxy_nativeProxyForUrl(JNIEnv* env, jclass, jstring url, jstring pac) {
  std::wstring u = Wide(env, url), p = Wide(env, pac);
  HINTERNET session = WinHttpOpen(L"Gesher", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (session == nullptr) return nullptr;
  WINHTTP_AUTOPROXY_OPTIONS options = {};
  if (!p.empty()) {
    options.dwFlags = WINHTTP_AUTOPROXY_CONFIG_URL;
    options.lpszAutoConfigUrl = p.c_str();
  } else {
    options.dwFlags = WINHTTP_AUTOPROXY_AUTO_DETECT;
    options.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
  }
  // In this process: the WinHTTP auto-proxy service may be off (Server images).
  options.dwFlags |= WINHTTP_AUTOPROXY_RUN_INPROCESS;
  options.fAutoLogonIfChallenged = TRUE;
  WINHTTP_PROXY_INFO info = {};
  jstring result = nullptr;
  if (WinHttpGetProxyForUrl(session, u.c_str(), &options, &info)) {
    std::string out;
    if (info.dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY && info.lpszProxy != nullptr) {
      // WinHTTP lists them separated by ';' or whitespace; "http=" prefixes are
      // possible too.
      std::string list = Utf8(info.lpszProxy);
      size_t i = 0;
      while (i < list.size()) {
        size_t j = list.find_first_of("; \t", i);
        if (j == std::string::npos) j = list.size();
        std::string one = list.substr(i, j - i);
        size_t eq = one.find('=');
        if (eq != std::string::npos) one = one.substr(eq + 1);
        size_t scheme = one.find("://");
        if (scheme != std::string::npos) one = one.substr(scheme + 3);
        if (!one.empty()) out += (out.empty() ? "PROXY " : "; PROXY ") + one;
        i = j + 1;
      }
    }
    if (out.empty()) out = "DIRECT";
    result = env->NewStringUTF(out.c_str());
    if (info.lpszProxy != nullptr) GlobalFree(info.lpszProxy);
    if (info.lpszProxyBypass != nullptr) GlobalFree(info.lpszProxyBypass);
  }
  WinHttpCloseHandle(session);
  return result;
}

// Waits up to timeout_ms for a change of the user's Internet Settings key.
jboolean WindowsProxy_nativeWaitForChange(JNIEnv*, jclass, jint timeout_ms) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", 0,
                    KEY_NOTIFY, &key) != ERROR_SUCCESS) {
    Sleep(static_cast<DWORD>(timeout_ms < 0 ? 15000 : timeout_ms));
    return JNI_FALSE;
  }
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  jboolean changed = JNI_FALSE;
  if (event != nullptr &&
      RegNotifyChangeKeyValue(key, TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME,
                              event, TRUE) == ERROR_SUCCESS) {
    DWORD r = WaitForSingleObject(event, timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
    if (r == WAIT_OBJECT_0) {
      Sleep(200);  // the settings are several values, written one after the other
      changed = JNI_TRUE;
    }
  }
  if (event != nullptr) CloseHandle(event);
  RegCloseKey(key);
  return changed;
}

const JNINativeMethod kMethods[] = {
    {"nativeConfig", "()Ljava/lang/String;", NATIVEHELPER_SYSV_JNI_ENTRY(WindowsProxy_nativeConfig)},
    {"nativeProxyForUrl", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
     NATIVEHELPER_SYSV_JNI_ENTRY(WindowsProxy_nativeProxyForUrl)},
    {"nativeWaitForChange", "(I)Z", NATIVEHELPER_SYSV_JNI_ENTRY(WindowsProxy_nativeWaitForChange)},
};

jint OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  jclass c = env->FindClass("com/gesher/net/WindowsProxy");
  if (c == nullptr) return JNI_ERR;
  if (env->RegisterNatives(c, kMethods, sizeof(kMethods) / sizeof(kMethods[0])) != JNI_OK) {
    return JNI_ERR;
  }
  env->DeleteLocalRef(c);
  return JNI_VERSION_1_6;
}

const gesher_builtin_symbol kGesherProxy[] = {
    {"JNI_OnLoad", reinterpret_cast<void*>(&OnLoad)},
};

[[maybe_unused]] const bool registered = [] {
  gesher_register_builtin_library("libgesher_proxy.so", kGesherProxy,
                                  sizeof(kGesherProxy) / sizeof(kGesherProxy[0]));
  return true;
}();

}  // namespace
