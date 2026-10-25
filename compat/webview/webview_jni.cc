// Gesher: com.gesher.app.webview.WebView2Host's natives (gesher-app.jar,
// app/java/com/gesher/app/webview/WebView2Host.java) over webview_host.h:
// android.webkit.WebView on WebView2 (docs/webview.md, ADR 0041).
//
// A builtin native library of dalvikvm, libgesher_webview.so
// (gesher_register_builtin_library): WebView2Host loads it with
// System.loadLibrary("gesher_webview"). The natives are JNICALL, which is
// System V under GESHER, as ART calls natives (ADR 0006); JNI_OnLoad is
// called as a C function, like libandroid_runtime's builtin one.

#include <jni.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include <gesher/builtin_library.h>

#include "webview_host.h"

using gesher::webview::Host;
using gesher::webview::HostOptions;
using gesher::webview::Listener;

namespace {

JavaVM* g_vm = nullptr;
jclass g_host_class = nullptr;  // com.gesher.app.webview.WebView2Host
jmethodID g_on_event = nullptr;
jmethodID g_on_navigation_starting = nullptr;
jmethodID g_on_resource_request = nullptr;
jmethodID g_on_bridge_call = nullptr;
jmethodID g_on_global = nullptr;

// The calling thread's JNIEnv, attaching it (as a daemon) the first time:
// the WebView thread.
JNIEnv* Env() {
  JNIEnv* env = nullptr;
  if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
  JavaVMAttachArgs args = {JNI_VERSION_1_6, const_cast<char*>("WebView2"), nullptr};
  if (g_vm->AttachCurrentThreadAsDaemon(&env, &args) != JNI_OK) return nullptr;
  return env;
}

std::wstring Str(JNIEnv* env, jstring s) {
  if (s == nullptr) return std::wstring();
  jsize n = env->GetStringLength(s);
  std::wstring out(static_cast<size_t>(n), L'\0');
  if (n > 0) env->GetStringRegion(s, 0, n, reinterpret_cast<jchar*>(&out[0]));
  return out;
}

jstring JStr(JNIEnv* env, const std::wstring& s) {
  return env->NewString(reinterpret_cast<const jchar*>(s.data()), static_cast<jsize>(s.size()));
}

void CheckException(JNIEnv* env) {
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
}

class JavaListener final : public Listener {
 public:
  JavaListener(JNIEnv* env, jobject peer) : peer_(env->NewGlobalRef(peer)) {}

  void OnEvent(int32_t type, int64_t id, int32_t arg, const std::wstring& a, const std::wstring& b,
               const std::wstring& c, const std::vector<uint8_t>* bytes) override {
    JNIEnv* env = Env();
    if (env == nullptr) return;
    jstring ja = JStr(env, a), jb = JStr(env, b), jc = JStr(env, c);
    jbyteArray data = nullptr;
    if (bytes != nullptr) {
      data = env->NewByteArray(static_cast<jsize>(bytes->size()));
      if (data != nullptr && !bytes->empty()) {
        env->SetByteArrayRegion(data, 0, static_cast<jsize>(bytes->size()),
                                reinterpret_cast<const jbyte*>(bytes->data()));
      }
    }
    env->CallVoidMethod(peer_, g_on_event, type, static_cast<jlong>(id), arg, ja, jb, jc, data);
    CheckException(env);
    env->DeleteLocalRef(ja);
    env->DeleteLocalRef(jb);
    env->DeleteLocalRef(jc);
    if (data != nullptr) env->DeleteLocalRef(data);
  }

  bool OnNavigationStarting(const std::wstring& url, bool user_initiated, bool redirect) override {
    JNIEnv* env = Env();
    if (env == nullptr) return false;
    jstring jurl = JStr(env, url);
    jboolean cancel = env->CallBooleanMethod(peer_, g_on_navigation_starting, jurl,
                                             user_initiated ? JNI_TRUE : JNI_FALSE,
                                             redirect ? JNI_TRUE : JNI_FALSE);
    CheckException(env);
    env->DeleteLocalRef(jurl);
    return cancel == JNI_TRUE;
  }

  void OnResourceRequest(int64_t id, const std::wstring& url, const std::wstring& method,
                         const std::wstring& headers, int32_t context, bool main_frame) override {
    JNIEnv* env = Env();
    if (env == nullptr) return;
    jstring ju = JStr(env, url), jm = JStr(env, method), jh = JStr(env, headers);
    env->CallVoidMethod(peer_, g_on_resource_request, static_cast<jlong>(id), ju, jm, jh, context,
                        main_frame ? JNI_TRUE : JNI_FALSE);
    CheckException(env);
    env->DeleteLocalRef(ju);
    env->DeleteLocalRef(jm);
    env->DeleteLocalRef(jh);
  }

  std::wstring OnBridgeCall(const std::wstring& object, const std::wstring& method,
                            const std::wstring& args_json) override {
    JNIEnv* env = Env();
    if (env == nullptr) return L"{\"e\":\"no Java thread\"}";
    jstring jo = JStr(env, object), jm = JStr(env, method), ja = JStr(env, args_json);
    auto result = static_cast<jstring>(env->CallObjectMethod(peer_, g_on_bridge_call, jo, jm, ja));
    CheckException(env);
    std::wstring out = result != nullptr ? Str(env, result) : L"{}";
    env->DeleteLocalRef(jo);
    env->DeleteLocalRef(jm);
    env->DeleteLocalRef(ja);
    if (result != nullptr) env->DeleteLocalRef(result);
    return out;
  }

  void OnDestroyed() override {
    if (JNIEnv* env = Env()) env->DeleteGlobalRef(peer_);
    delete this;
  }

 private:
  jobject peer_;
};

void GlobalCallback(int32_t type, int64_t id, int32_t arg, const std::wstring& a) {
  JNIEnv* env = Env();
  if (env == nullptr || g_host_class == nullptr) return;
  jstring ja = JStr(env, a);
  env->CallStaticVoidMethod(g_host_class, g_on_global, type, static_cast<jlong>(id), arg, ja);
  CheckException(env);
  env->DeleteLocalRef(ja);
}

Host* Get(jlong handle) { return reinterpret_cast<Host*>(handle); }

HWND Hwnd(jlong hwnd) { return reinterpret_cast<HWND>(static_cast<intptr_t>(hwnd)); }

// --- Natives ------------------------------------------------------------------

// The installed runtime's version, or "!0x........" (the loader's error).
jstring JNICALL WebView2Host_runtimeVersion(JNIEnv* env, jclass) {
  HRESULT hr = S_OK;
  std::wstring version = Host::RuntimeVersion(&hr);
  if (version.empty()) {
    wchar_t buf[16];
    swprintf(buf, 16, L"!0x%08lx", static_cast<unsigned long>(hr));
    version = buf;
  }
  return JStr(env, version);
}

jstring JNICALL WebView2Host_loaderKind(JNIEnv* env, jclass) {
  return env->NewStringUTF(gesher::webview::LoaderKind());
}

void JNICALL WebView2Host_configure(JNIEnv* env, jclass, jstring user_data, jstring args) {
  Host::Configure(Str(env, user_data), Str(env, args));
}

jlong JNICALL WebView2Host_create(JNIEnv* env, jclass, jobject peer, jlong parent, jdouble scale,
                                  jint background) {
  HostOptions options;
  options.parent = Hwnd(parent);
  options.scale = scale > 0 ? scale : 1.0;
  options.background = static_cast<uint32_t>(background);
  return reinterpret_cast<jlong>(Host::Create(new JavaListener(env, peer), options));
}

void JNICALL WebView2Host_destroy(JNIEnv*, jclass, jlong h) { Get(h)->Destroy(); }

std::vector<uint8_t> Bytes(JNIEnv* env, jbyteArray a) {
  std::vector<uint8_t> bytes;
  if (a != nullptr) {
    bytes.resize(static_cast<size_t>(env->GetArrayLength(a)));
    if (!bytes.empty()) {
      env->GetByteArrayRegion(a, 0, static_cast<jsize>(bytes.size()),
                              reinterpret_cast<jbyte*>(bytes.data()));
    }
  }
  return bytes;
}

void JNICALL WebView2Host_navigate(JNIEnv* env, jclass, jlong h, jstring url, jstring headers,
                                   jstring method, jbyteArray body) {
  std::wstring m = Str(env, method);
  Get(h)->Navigate(Str(env, url), Str(env, headers), m.empty() ? L"GET" : m, Bytes(env, body));
}

void JNICALL WebView2Host_postWebMessage(JNIEnv* env, jclass, jlong h, jstring json) {
  Get(h)->PostWebMessage(Str(env, json));
}

void JNICALL WebView2Host_callDevTools(JNIEnv* env, jclass, jlong h, jstring method,
                                       jstring params, jlong cb) {
  Get(h)->CallDevTools(Str(env, method), Str(env, params), cb);
}

void JNICALL WebView2Host_listenDevTools(JNIEnv* env, jclass, jlong h, jstring event) {
  Get(h)->ListenDevTools(Str(env, event));
}

void JNICALL WebView2Host_completePermission(JNIEnv*, jclass, jlong h, jlong id, jint state,
                                             jboolean remember) {
  Get(h)->CompletePermission(id, state, remember == JNI_TRUE);
}

void JNICALL WebView2Host_completeNewWindow(JNIEnv*, jclass, jlong h, jlong id, jlong window) {
  Get(h)->CompleteNewWindow(id, window != 0 ? Get(window) : nullptr);
}

void JNICALL WebView2Host_completeBasicAuth(JNIEnv* env, jclass, jlong h, jlong id,
                                            jboolean proceed, jstring user, jstring password) {
  Get(h)->CompleteBasicAuth(id, proceed == JNI_TRUE, Str(env, user), Str(env, password));
}

void JNICALL WebView2Host_completeClientCert(JNIEnv*, jclass, jlong h, jlong id, jint choice) {
  Get(h)->CompleteClientCert(id, choice);
}

void JNICALL WebView2Host_completeServerCertError(JNIEnv*, jclass, jlong h, jlong id,
                                                  jboolean allow) {
  Get(h)->CompleteServerCertError(id, allow == JNI_TRUE);
}

void JNICALL WebView2Host_find(JNIEnv* env, jclass, jlong h, jint op, jstring term,
                               jboolean case_sensitive, jboolean whole_word) {
  Get(h)->Find(op, Str(env, term), case_sensitive == JNI_TRUE, whole_word == JNI_TRUE);
}

void JNICALL WebView2Host_showFileDialog(JNIEnv* env, jclass, jlong h, jlong cb, jboolean multiple,
                                         jstring title, jstring filters) {
  Get(h)->ShowFileDialog(cb, multiple == JNI_TRUE, Str(env, title), Str(env, filters));
}

// A Java path (drive-less: /a/b is \a\b on the current drive, ADR 0010) as a
// full Windows path.
jstring JNICALL WebView2Host_windowsPath(JNIEnv* env, jclass, jstring path) {
  std::wstring p = Str(env, path);
  for (wchar_t& ch : p) {
    if (ch == L'/') ch = L'\\';
  }
  wchar_t full[MAX_PATH * 2];
  DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 2, full, nullptr);
  if (n > 0 && n < MAX_PATH * 2) p.assign(full, n);
  return JStr(env, p);
}

void JNICALL WebView2Host_navigateToString(JNIEnv* env, jclass, jlong h, jstring html) {
  Get(h)->NavigateToString(Str(env, html));
}

void JNICALL WebView2Host_run(JNIEnv*, jclass, jlong h, jint command) {
  Get(h)->Run(static_cast<Host::Command>(command));
}

void JNICALL WebView2Host_setBounds(JNIEnv* env, jclass, jlong h, jlong parent, jint x, jint y,
                                    jint width, jint height, jint clip_x, jint clip_y,
                                    jint clip_width, jint clip_height, jboolean visible,
                                    jintArray shapes) {
  // shapes: kind, left, top, right, bottom, radius, point count, then x, y per
  // point, for each (Host::Shape; GesherWebViewProvider.findHoles).
  std::vector<Host::Shape> out;
  jsize n = shapes != nullptr ? env->GetArrayLength(shapes) : 0;
  if (n >= 7) {
    std::vector<jint> v(n);
    env->GetIntArrayRegion(shapes, 0, n, v.data());
    for (jsize i = 0; i + 6 < n;) {
      Host::Shape s;
      s.kind = v[i];
      s.rect = {v[i + 1], v[i + 2], v[i + 3], v[i + 4]};
      s.radius = v[i + 5];
      jint points = v[i + 6];
      i += 7;
      for (jint k = 0; k < points && i + 1 < n; ++k, i += 2) s.points.push_back({v[i], v[i + 1]});
      out.push_back(std::move(s));
    }
  }
  Get(h)->SetBounds(Hwnd(parent), x, y, width, height, clip_x, clip_y, clip_width, clip_height,
                    visible == JNI_TRUE, out);
}

void JNICALL WebView2Host_park(JNIEnv*, jclass, jlong h, jlong id) { Get(h)->Park(id); }

void JNICALL WebView2Host_setSettings(JNIEnv* env, jclass, jlong h, jboolean javascript,
                                      jboolean zoom_controls, jboolean pinch_zoom,
                                      jboolean devtools, jboolean intercept_all,
                                      jboolean reputation_checking, jstring user_agent) {
  Host::Settings s;
  s.javascript = javascript == JNI_TRUE;
  s.zoom_controls = zoom_controls == JNI_TRUE;
  s.pinch_zoom = pinch_zoom == JNI_TRUE;
  s.devtools = devtools == JNI_TRUE;
  s.intercept_all = intercept_all == JNI_TRUE;
  s.reputation_checking = reputation_checking == JNI_TRUE;
  s.user_agent = Str(env, user_agent);
  Get(h)->SetSettings(s);
}

void JNICALL WebView2Host_setZoom(JNIEnv*, jclass, jlong h, jdouble factor) {
  Get(h)->SetZoom(factor);
}

void JNICALL WebView2Host_setBackground(JNIEnv*, jclass, jlong h, jint argb) {
  Get(h)->SetBackground(static_cast<uint32_t>(argb));
}

void JNICALL WebView2Host_executeScript(JNIEnv* env, jclass, jlong h, jstring script, jlong cb) {
  Get(h)->ExecuteScript(Str(env, script), cb);
}

void JNICALL WebView2Host_setDocumentScript(JNIEnv* env, jclass, jlong h, jstring script) {
  Get(h)->SetDocumentScript(Str(env, script));
}

void JNICALL WebView2Host_respond(JNIEnv* env, jclass, jlong h, jlong id, jint status,
                                  jstring reason, jstring headers, jbyteArray body) {
  std::vector<uint8_t> bytes;
  if (body != nullptr) {
    bytes.resize(static_cast<size_t>(env->GetArrayLength(body)));
    if (!bytes.empty()) {
      env->GetByteArrayRegion(body, 0, static_cast<jsize>(bytes.size()),
                              reinterpret_cast<jbyte*>(bytes.data()));
    }
  }
  Get(h)->RespondToRequest(id, status, Str(env, reason), Str(env, headers), std::move(bytes));
}

void JNICALL WebView2Host_completeDialog(JNIEnv* env, jclass, jlong h, jlong id, jboolean accept,
                                         jstring text) {
  Get(h)->CompleteDialog(id, accept == JNI_TRUE, Str(env, text));
}

void JNICALL WebView2Host_capturePreview(JNIEnv*, jclass, jlong h, jlong cb) {
  Get(h)->CapturePreview(cb);
}

void JNICALL WebView2Host_setCookie(JNIEnv* env, jclass, jstring url, jstring name, jstring value,
                                    jstring domain, jstring path, jdouble expires,
                                    jboolean secure, jboolean http_only, jlong cb) {
  Host::Cookie c;
  c.name = Str(env, name);
  c.value = Str(env, value);
  c.domain = Str(env, domain);
  c.path = Str(env, path);
  c.expires = expires;
  c.secure = secure == JNI_TRUE;
  c.http_only = http_only == JNI_TRUE;
  Host::SetCookie(Str(env, url), c, cb);
}

void JNICALL WebView2Host_getCookies(JNIEnv* env, jclass, jstring url, jlong cb) {
  Host::GetCookies(Str(env, url), cb);
}

void JNICALL WebView2Host_deleteCookies(JNIEnv*, jclass, jint which, jlong cb) {
  Host::DeleteCookies(which, cb);
}

void JNICALL WebView2Host_clearBrowsingData(JNIEnv*, jclass, jint kinds, jlong cb) {
  Host::ClearBrowsingData(kinds, cb);
}

const JNINativeMethod kMethods[] = {
    {"nRuntimeVersion", "()Ljava/lang/String;", reinterpret_cast<void*>(&WebView2Host_runtimeVersion)},
    {"nLoaderKind", "()Ljava/lang/String;", reinterpret_cast<void*>(&WebView2Host_loaderKind)},
    {"nConfigure", "(Ljava/lang/String;Ljava/lang/String;)V",
     reinterpret_cast<void*>(&WebView2Host_configure)},
    {"nCreate", "(Lcom/gesher/app/webview/WebView2Host;JDI)J",
     reinterpret_cast<void*>(&WebView2Host_create)},
    {"nDestroy", "(J)V", reinterpret_cast<void*>(&WebView2Host_destroy)},
    {"nNavigate", "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;[B)V",
     reinterpret_cast<void*>(&WebView2Host_navigate)},
    {"nPostWebMessage", "(JLjava/lang/String;)V",
     reinterpret_cast<void*>(&WebView2Host_postWebMessage)},
    {"nCallDevTools", "(JLjava/lang/String;Ljava/lang/String;J)V",
     reinterpret_cast<void*>(&WebView2Host_callDevTools)},
    {"nListenDevTools", "(JLjava/lang/String;)V",
     reinterpret_cast<void*>(&WebView2Host_listenDevTools)},
    {"nCompletePermission", "(JJIZ)V", reinterpret_cast<void*>(&WebView2Host_completePermission)},
    {"nCompleteNewWindow", "(JJJ)V", reinterpret_cast<void*>(&WebView2Host_completeNewWindow)},
    {"nCompleteBasicAuth", "(JJZLjava/lang/String;Ljava/lang/String;)V",
     reinterpret_cast<void*>(&WebView2Host_completeBasicAuth)},
    {"nCompleteClientCert", "(JJI)V", reinterpret_cast<void*>(&WebView2Host_completeClientCert)},
    {"nCompleteServerCertError", "(JJZ)V",
     reinterpret_cast<void*>(&WebView2Host_completeServerCertError)},
    {"nFind", "(JILjava/lang/String;ZZ)V", reinterpret_cast<void*>(&WebView2Host_find)},
    {"nShowFileDialog", "(JJZLjava/lang/String;Ljava/lang/String;)V",
     reinterpret_cast<void*>(&WebView2Host_showFileDialog)},
    {"nWindowsPath", "(Ljava/lang/String;)Ljava/lang/String;",
     reinterpret_cast<void*>(&WebView2Host_windowsPath)},
    {"nNavigateToString", "(JLjava/lang/String;)V",
     reinterpret_cast<void*>(&WebView2Host_navigateToString)},
    {"nRun", "(JI)V", reinterpret_cast<void*>(&WebView2Host_run)},
    {"nSetBounds", "(JJIIIIIIIIZ[I)V", reinterpret_cast<void*>(&WebView2Host_setBounds)},
    {"nPark", "(JJ)V", reinterpret_cast<void*>(&WebView2Host_park)},
    {"nSetSettings", "(JZZZZZZLjava/lang/String;)V",
     reinterpret_cast<void*>(&WebView2Host_setSettings)},
    {"nSetZoom", "(JD)V", reinterpret_cast<void*>(&WebView2Host_setZoom)},
    {"nSetBackground", "(JI)V", reinterpret_cast<void*>(&WebView2Host_setBackground)},
    {"nExecuteScript", "(JLjava/lang/String;J)V",
     reinterpret_cast<void*>(&WebView2Host_executeScript)},
    {"nSetDocumentScript", "(JLjava/lang/String;)V",
     reinterpret_cast<void*>(&WebView2Host_setDocumentScript)},
    {"nRespond", "(JJILjava/lang/String;Ljava/lang/String;[B)V",
     reinterpret_cast<void*>(&WebView2Host_respond)},
    {"nCompleteDialog", "(JJZLjava/lang/String;)V",
     reinterpret_cast<void*>(&WebView2Host_completeDialog)},
    {"nCapturePreview", "(JJ)V", reinterpret_cast<void*>(&WebView2Host_capturePreview)},
    {"nSetCookie",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
     "Ljava/lang/String;DZZJ)V",
     reinterpret_cast<void*>(&WebView2Host_setCookie)},
    {"nGetCookies", "(Ljava/lang/String;J)V", reinterpret_cast<void*>(&WebView2Host_getCookies)},
    {"nDeleteCookies", "(IJ)V", reinterpret_cast<void*>(&WebView2Host_deleteCookies)},
    {"nClearBrowsingData", "(IJ)V", reinterpret_cast<void*>(&WebView2Host_clearBrowsingData)},
};

}  // namespace

extern "C" jint JNI_OnLoad_gesher_webview(JavaVM* vm, void*) {
  g_vm = vm;
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  jclass c = env->FindClass("com/gesher/app/webview/WebView2Host");
  if (c == nullptr) return JNI_ERR;
  g_host_class = static_cast<jclass>(env->NewGlobalRef(c));
  g_on_event = env->GetMethodID(c, "onEvent",
                                "(IJILjava/lang/String;Ljava/lang/String;Ljava/lang/String;[B)V");
  g_on_navigation_starting =
      env->GetMethodID(c, "onNavigationStarting", "(Ljava/lang/String;ZZ)Z");
  g_on_resource_request = env->GetMethodID(
      c, "onResourceRequest", "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;IZ)V");
  g_on_bridge_call = env->GetMethodID(
      c, "onBridgeCall",
      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
  g_on_global = env->GetStaticMethodID(c, "onGlobal", "(IJILjava/lang/String;)V");
  if (g_on_event == nullptr || g_on_navigation_starting == nullptr ||
      g_on_resource_request == nullptr || g_on_bridge_call == nullptr || g_on_global == nullptr) {
    return JNI_ERR;
  }
  if (env->RegisterNatives(c, kMethods, sizeof(kMethods) / sizeof(kMethods[0])) < 0) {
    return JNI_ERR;
  }
  env->DeleteLocalRef(c);
  Host::SetGlobalCallback(&GlobalCallback);
  return JNI_VERSION_1_6;
}

namespace {

const gesher_builtin_symbol kWebView[] = {
    {"JNI_OnLoad", reinterpret_cast<void*>(&JNI_OnLoad_gesher_webview)},
};

[[maybe_unused]] const bool registered = [] {
  gesher_register_builtin_library("libgesher_webview.so", kWebView,
                                  sizeof(kWebView) / sizeof(kWebView[0]));
  return true;
}();

}  // namespace
