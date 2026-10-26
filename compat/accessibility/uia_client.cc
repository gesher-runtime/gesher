// Gesher: WebView2's UI Automation tree for Android's accessibility (ADR
// 0058): com.gesher.app.accessibility.UiaClient's natives. A WebView's
// AccessibilityNodeProvider (GesherWebViewProvider) shows the page's
// accessibility nodes as virtual views; they are read here, as a UIA client,
// from the WebView2 window (its container's subtree: Chromium's UIA tree of
// the page) and acted on (Invoke, Toggle, SetFocus, SetValue, ScrollIntoView).
//
// UIA client calls run on a thread of their own (an MTA), never on the app's
// threads: the app's UI thread waits (bounded) for the answer, and nothing
// UIA does there can call back into a thread that waits.

#undef WIN32_LEAN_AND_MEAN
#undef NOGDI
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <uiautomation.h>

#include <chrono>
#include <atomic>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <jni.h>
#include <nativehelper/jni_macros.h>

namespace {

// --- The UIA thread -----------------------------------------------------------

std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<std::function<void()>> g_jobs;
bool g_started = false;
IUIAutomation* g_uia = nullptr;  // the UIA thread's
std::atomic<const char*> g_phase{"starting"};

DWORD WINAPI UiaMain(void*) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const CLSID clsid = {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
  CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                   reinterpret_cast<void**>(&g_uia));
  while (true) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lock(g_mutex);
      g_cv.wait(lock, [] { return !g_jobs.empty(); });
      job = std::move(g_jobs.front());
      g_jobs.pop_front();
    }
    job();
  }
  return 0;
}

// Runs fn on the UIA thread, waiting up to timeout_ms; false when it did not
// finish in time (it still runs, later).
bool RunOnUiaThread(std::function<void()> fn, DWORD timeout_ms) {
  struct Done {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
  };
  auto done = std::make_shared<Done>();
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_started) {
      g_started = true;
      HANDLE t = CreateThread(nullptr, 0, UiaMain, nullptr, 0, nullptr);
      if (t != nullptr) CloseHandle(t);
    }
    g_jobs.push_back([fn, done] {
      fn();
      std::lock_guard<std::mutex> l(done->m);
      done->done = true;
      done->cv.notify_all();
    });
  }
  g_cv.notify_all();
  std::unique_lock<std::mutex> lock(done->m);
  return done->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return done->done; });
}

// --- Snapshots --------------------------------------------------------------------

// The last snapshot's elements (the UIA thread's), for actions on them.
std::vector<IUIAutomationElement*> g_elements;
int g_generation = 0;

void ReleaseElements() {
  for (IUIAutomationElement* e : g_elements) e->Release();
  g_elements.clear();
}

std::wstring Clean(BSTR b) {
  std::wstring s = b != nullptr ? std::wstring(b, SysStringLen(b)) : std::wstring();
  for (wchar_t& c : s) {
    if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
  }
  return s;
}

bool CachedBool(IUIAutomationElement* e, PROPERTYID id) {
  VARIANT v;
  VariantInit(&v);
  bool on = SUCCEEDED(e->GetCachedPropertyValue(id, &v)) && v.vt == VT_BOOL && v.boolVal;
  VariantClear(&v);
  return on;
}

int CachedInt(IUIAutomationElement* e, PROPERTYID id, int fallback) {
  VARIANT v;
  VariantInit(&v);
  int out = fallback;
  if (SUCCEEDED(e->GetCachedPropertyValue(id, &v)) && v.vt == VT_I4) out = v.lVal;
  VariantClear(&v);
  return out;
}

std::wstring CachedString(IUIAutomationElement* e, PROPERTYID id) {
  VARIANT v;
  VariantInit(&v);
  std::wstring out;
  if (SUCCEEDED(e->GetCachedPropertyValue(id, &v)) && v.vt == VT_BSTR) out = Clean(v.bstrVal);
  VariantClear(&v);
  return out;
}

// Node flags, as uia_bridge.cc's (and UiaClient.java's).
enum : uint32_t {
  kEnabled = 1, kFocusable = 2, kFocused = 4, kClickable = 8, kCheckable = 16, kChecked = 32,
  kEditable = 64, kPassword = 128, kVisible = 256, kScrollable = 512, kHeading = 1024,
  kSelected = 2048,
};

// "gen\t<n>" then per element:
// "id\tparent\tcontrolType\tname\tvalue\tl t r b\tflags\tautomationId\thelp"
// (screen coordinates; parent -1 for the top level).
std::wstring Snapshot(HWND hwnd, int max_nodes) {
  g_phase.store("snapshot setup");
  ReleaseElements();
  g_generation++;
  std::wstring out = L"gen\t" + std::to_wstring(g_generation) + L"\n";
  if (g_uia == nullptr) return out;
  IUIAutomationCacheRequest* cache = nullptr;
  if (FAILED(g_uia->CreateCacheRequest(&cache))) return out;
  const PROPERTYID props[] = {
      UIA_NamePropertyId, UIA_ControlTypePropertyId, UIA_BoundingRectanglePropertyId,
      UIA_IsEnabledPropertyId, UIA_HasKeyboardFocusPropertyId, UIA_IsKeyboardFocusablePropertyId,
      UIA_IsPasswordPropertyId, UIA_AutomationIdPropertyId, UIA_HelpTextPropertyId,
      UIA_IsOffscreenPropertyId, UIA_IsInvokePatternAvailablePropertyId,
      UIA_IsTogglePatternAvailablePropertyId, UIA_ToggleToggleStatePropertyId,
      UIA_IsValuePatternAvailablePropertyId, UIA_ValueValuePropertyId,
      UIA_ValueIsReadOnlyPropertyId, UIA_HeadingLevelPropertyId,
      UIA_IsScrollPatternAvailablePropertyId, UIA_SelectionItemIsSelectedPropertyId,
  };
  for (PROPERTYID p : props) cache->AddProperty(p);
  IUIAutomationTreeWalker* walker = nullptr;
  g_uia->get_ControlViewWalker(&walker);
  IUIAutomationElement* root = nullptr;
  g_phase.store("ElementFromHandleBuildCache");
  if (walker == nullptr || FAILED(g_uia->ElementFromHandleBuildCache(hwnd, cache, &root)) ||
      root == nullptr) {
    if (walker != nullptr) walker->Release();
    cache->Release();
    return out;
  }
  struct Item {
    IUIAutomationElement* element;
    int parent;
    int depth;
  };
  std::vector<Item> stack;
  // The container itself (Gesher's window) is not a node: its subtree is.
  IUIAutomationElement* child = nullptr;
  std::vector<IUIAutomationElement*> top;
  g_phase.store("first child");
  walker->GetFirstChildElementBuildCache(root, cache, &child);
  while (child != nullptr) {
    top.push_back(child);
    IUIAutomationElement* next = nullptr;
    walker->GetNextSiblingElementBuildCache(child, cache, &next);
    child = next;
  }
  for (auto it = top.rbegin(); it != top.rend(); ++it) stack.push_back({*it, -1, 0});
  root->Release();
  while (!stack.empty() && static_cast<int>(g_elements.size()) < max_nodes) {
    g_phase.store("element properties");
    Item item = stack.back();
    stack.pop_back();
    IUIAutomationElement* e = item.element;
    int id = static_cast<int>(g_elements.size());
    g_elements.push_back(e);
    BSTR name = nullptr;
    e->get_CachedName(&name);
    RECT r = {0, 0, 0, 0};
    e->get_CachedBoundingRectangle(&r);
    int type = 0;
    e->get_CachedControlType(&type);
    uint32_t flags = 0;
    if (CachedBool(e, UIA_IsEnabledPropertyId)) flags |= kEnabled;
    if (CachedBool(e, UIA_IsKeyboardFocusablePropertyId)) flags |= kFocusable;
    if (CachedBool(e, UIA_HasKeyboardFocusPropertyId)) flags |= kFocused;
    if (CachedBool(e, UIA_IsInvokePatternAvailablePropertyId)) flags |= kClickable;
    if (CachedBool(e, UIA_IsTogglePatternAvailablePropertyId)) {
      flags |= kCheckable;
      if (CachedInt(e, UIA_ToggleToggleStatePropertyId, 0) == ToggleState_On) flags |= kChecked;
    }
    bool value = CachedBool(e, UIA_IsValuePatternAvailablePropertyId);
    if (value && !CachedBool(e, UIA_ValueIsReadOnlyPropertyId)) flags |= kEditable;
    if (CachedBool(e, UIA_IsPasswordPropertyId)) flags |= kPassword;
    if (!CachedBool(e, UIA_IsOffscreenPropertyId)) flags |= kVisible;
    if (CachedBool(e, UIA_IsScrollPatternAvailablePropertyId)) flags |= kScrollable;
    // HeadingLevel_None is 80050 (not in mingw-w64's header).
    if (CachedInt(e, UIA_HeadingLevelPropertyId, 80050) != 80050) {
      flags |= kHeading;
    }
    if (CachedBool(e, UIA_SelectionItemIsSelectedPropertyId)) flags |= kSelected;
    std::wstring v = value ? CachedString(e, UIA_ValueValuePropertyId) : std::wstring();
    out += std::to_wstring(id) + L"\t" + std::to_wstring(item.parent) + L"\t" +
           std::to_wstring(type) + L"\t" + Clean(name) + L"\t" + v + L"\t" +
           std::to_wstring(r.left) + L" " + std::to_wstring(r.top) + L" " +
           std::to_wstring(r.right) + L" " + std::to_wstring(r.bottom) + L"\t" +
           std::to_wstring(flags) + L"\t" + CachedString(e, UIA_AutomationIdPropertyId) + L"\t" +
           CachedString(e, UIA_HelpTextPropertyId) + L"\n";
    SysFreeString(name);
    if (item.depth >= 64) continue;
    std::vector<IUIAutomationElement*> kids;
    IUIAutomationElement* c = nullptr;
    g_phase.store("element children");
    walker->GetFirstChildElementBuildCache(e, cache, &c);
    while (c != nullptr) {
      kids.push_back(c);
      IUIAutomationElement* next = nullptr;
      walker->GetNextSiblingElementBuildCache(c, cache, &next);
      c = next;
    }
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) stack.push_back({*it, id, item.depth + 1});
  }
  for (Item& left : stack) left.element->Release();
  walker->Release();
  cache->Release();
  g_phase.store("idle");
  return out;
}

// action: 1 click (Invoke, else Toggle, else SelectionItem.Select, else the
// legacy default action), 2 focus, 3 set the value, 5 scroll into view.
bool Act(int generation, int id, int action, const std::wstring& text) {
  if (generation != g_generation || id < 0 || id >= static_cast<int>(g_elements.size())) return false;
  IUIAutomationElement* e = g_elements[static_cast<size_t>(id)];
  switch (action) {
    case 1: {
      IUIAutomationInvokePattern* invoke = nullptr;
      if (SUCCEEDED(e->GetCurrentPatternAs(UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern),
                                          reinterpret_cast<void**>(&invoke))) && invoke != nullptr) {
        HRESULT hr = invoke->Invoke();
        invoke->Release();
        return SUCCEEDED(hr);
      }
      IUIAutomationTogglePattern* toggle = nullptr;
      if (SUCCEEDED(e->GetCurrentPatternAs(UIA_TogglePatternId, __uuidof(IUIAutomationTogglePattern),
                                          reinterpret_cast<void**>(&toggle))) && toggle != nullptr) {
        HRESULT hr = toggle->Toggle();
        toggle->Release();
        return SUCCEEDED(hr);
      }
      IUIAutomationSelectionItemPattern* select = nullptr;
      if (SUCCEEDED(e->GetCurrentPatternAs(UIA_SelectionItemPatternId,
                                          __uuidof(IUIAutomationSelectionItemPattern),
                                          reinterpret_cast<void**>(&select))) && select != nullptr) {
        HRESULT hr = select->Select();
        select->Release();
        return SUCCEEDED(hr);
      }
      IUIAutomationLegacyIAccessiblePattern* legacy = nullptr;
      if (SUCCEEDED(e->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId,
                                          __uuidof(IUIAutomationLegacyIAccessiblePattern),
                                          reinterpret_cast<void**>(&legacy))) && legacy != nullptr) {
        HRESULT hr = legacy->DoDefaultAction();
        legacy->Release();
        return SUCCEEDED(hr);
      }
      return false;
    }
    case 2:
      return SUCCEEDED(e->SetFocus());
    case 3: {
      IUIAutomationValuePattern* value = nullptr;
      if (FAILED(e->GetCurrentPatternAs(UIA_ValuePatternId, __uuidof(IUIAutomationValuePattern),
                                       reinterpret_cast<void**>(&value))) || value == nullptr) {
        return false;
      }
      BSTR b = SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
      HRESULT hr = value->SetValue(b);
      SysFreeString(b);
      value->Release();
      return SUCCEEDED(hr);
    }
    case 5: {
      IUIAutomationScrollItemPattern* scroll = nullptr;
      if (FAILED(e->GetCurrentPatternAs(UIA_ScrollItemPatternId, __uuidof(IUIAutomationScrollItemPattern),
                                       reinterpret_cast<void**>(&scroll))) || scroll == nullptr) {
        return false;
      }
      HRESULT hr = scroll->ScrollIntoView();
      scroll->Release();
      return SUCCEEDED(hr);
    }
    default:
      return false;
  }
}

// --- JNI ------------------------------------------------------------------------

std::wstring Str(JNIEnv* env, jstring s) {
  if (s == nullptr) return std::wstring();
  jsize n = env->GetStringLength(s);
  std::wstring out(static_cast<size_t>(n), L'\0');
  if (n > 0) env->GetStringRegion(s, 0, n, reinterpret_cast<jchar*>(&out[0]));
  return out;
}

jstring UiaClient_nativeSnapshot(JNIEnv* env, jclass, jlong hwnd, jint max_nodes, jint timeout_ms) {
  auto result = std::make_shared<std::wstring>();
  HWND h = reinterpret_cast<HWND>(static_cast<intptr_t>(hwnd));
  if (!RunOnUiaThread([h, max_nodes, result] { *result = Snapshot(h, max_nodes); },
                      static_cast<DWORD>(timeout_ms))) {
    std::fprintf(stderr, "gesher-a11y: UIA snapshot of %p timed out at %s\n",
                 static_cast<void*>(h), g_phase.load());
    return nullptr;
  }
  return env->NewString(reinterpret_cast<const jchar*>(result->data()),
                        static_cast<jsize>(result->size()));
}

jboolean UiaClient_nativeAct(JNIEnv* env, jclass, jint generation, jint id, jint action,
                             jstring text) {
  auto ok = std::make_shared<bool>(false);
  std::wstring t = Str(env, text);
  if (!RunOnUiaThread([=] { *ok = Act(generation, id, action, t); }, 3000)) return JNI_FALSE;
  return *ok ? JNI_TRUE : JNI_FALSE;
}

// The client area's origin on the screen: x << 32 | y (unsigned 32 bits each).
jlong UiaClient_nativeClientOrigin(JNIEnv*, jclass, jlong hwnd) {
  POINT p = {0, 0};
  ClientToScreen(reinterpret_cast<HWND>(static_cast<intptr_t>(hwnd)), &p);
  return (static_cast<jlong>(static_cast<uint32_t>(p.x)) << 32) | static_cast<uint32_t>(p.y);
}

const JNINativeMethod kMethods[] = {
    {"nativeClientOrigin", "(J)J", NATIVEHELPER_SYSV_JNI_ENTRY(UiaClient_nativeClientOrigin)},
    {"nativeSnapshot", "(JII)Ljava/lang/String;", NATIVEHELPER_SYSV_JNI_ENTRY(UiaClient_nativeSnapshot)},
    {"nativeAct", "(IIILjava/lang/String;)Z", NATIVEHELPER_SYSV_JNI_ENTRY(UiaClient_nativeAct)},
};

}  // namespace

jint RegisterUiaClientNatives(JNIEnv* env) {
  jclass c = env->FindClass("com/gesher/app/accessibility/UiaClient");
  if (c == nullptr) return JNI_ERR;
  jint r = env->RegisterNatives(c, kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
  env->DeleteLocalRef(c);
  return r == JNI_OK ? JNI_OK : JNI_ERR;
}
