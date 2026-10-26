// Gesher: Android's accessibility tree for Windows' UI Automation (ADR 0058,
// docs/services.md "Accessibility"): the reverse bridge. A Windows screen
// reader (Narrator) or any UIA client reads an app's Android views through
// providers this file serves for the app's Win32 windows (class
// GesherWindow): WM_GETOBJECT(UiaRootObjectId) gets a fragment root whose
// elements are the window's AccessibilityNodeInfos, which
// com.gesher.app.accessibility.UiaBridge reads through the window's
// ViewRootImpl (its direct accessibility connection, the
// AccessibilityInteractionController). An element that hosts a child window
// (a WebView's WebView2 container) returns that window's host provider, so
// UIA puts WebView2's own tree there.
//
// The windows are subclassed (SetWindowLongPtr) from a helper thread that a
// WinEvent hook (EVENT_OBJECT_CREATE, this process) wakes for each new
// window; the window's thread answers WM_GETOBJECT. Providers are
// free-threaded: UIA calls them on its threads, and they call Java there.
// Built into dalvikvm as the builtin library libgesher_a11y.so.

#undef WIN32_LEAN_AND_MEAN
#undef NOGDI
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <uiautomation.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <gesher/builtin_library.h>
#include <jni.h>
#include <nativehelper/jni_macros.h>

namespace {

JavaVM* g_vm = nullptr;
jclass g_bridge = nullptr;  // com.gesher.app.accessibility.UiaBridge
jmethodID g_node = nullptr, g_node_at = nullptr, g_focused = nullptr, g_perform = nullptr;

// The calling thread's JNIEnv for one call into Java. UI Automation calls the
// providers on threads of its own, which end whenever UIA likes: a thread
// attached here is detached again (ART aborts when an attached thread exits).
class ScopedEnv {
 public:
  ScopedEnv() {
    if (g_vm == nullptr) return;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6) == JNI_OK) return;
    env_ = nullptr;
    JavaVMAttachArgs args = {JNI_VERSION_1_6, const_cast<char*>("UIA"), nullptr};
    if (g_vm->AttachCurrentThreadAsDaemon(&env_, &args) == JNI_OK) {
      attached_ = true;
    } else {
      env_ = nullptr;
    }
  }
  ~ScopedEnv() {
    if (attached_) g_vm->DetachCurrentThread();
  }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;
  JNIEnv* get() const { return env_; }

 private:
  JNIEnv* env_ = nullptr;
  bool attached_ = false;
};

void Clear(JNIEnv* env) {
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
}

std::wstring Str(JNIEnv* env, jstring s) {
  if (s == nullptr) return std::wstring();
  jsize n = env->GetStringLength(s);
  std::wstring out(static_cast<size_t>(n), L'\0');
  if (n > 0) env->GetStringRegion(s, 0, n, reinterpret_cast<jchar*>(&out[0]));
  return out;
}

// --- A node, as UiaBridge.node describes it ------------------------------------

enum Flag : uint32_t {
  kEnabled = 1, kFocusable = 2, kFocused = 4, kClickable = 8, kCheckable = 16, kChecked = 32,
  kEditable = 64, kPassword = 128, kVisible = 256, kScrollable = 512, kHeading = 1024,
  kSelected = 2048, kLongClickable = 4096,
};

struct Node {
  bool ok = false;
  int64_t id = 0, parent = 0;
  std::wstring class_name, name, value, help, automation_id;
  uint32_t flags = 0;
  RECT bounds = {0, 0, 0, 0};  // client coordinates of the window
  std::vector<int64_t> children;
  HWND hosted = nullptr;       // a child window shown in this node (a WebView)
  int control_type = UIA_CustomControlTypeId;
};

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
  std::vector<std::wstring> out;
  size_t i = 0;
  while (true) {
    size_t j = s.find(sep, i);
    out.push_back(s.substr(i, j == std::wstring::npos ? std::wstring::npos : j - i));
    if (j == std::wstring::npos) break;
    i = j + 1;
  }
  return out;
}

int64_t ToId(const std::wstring& s) { return s.empty() ? 0 : _wcstoi64(s.c_str(), nullptr, 10); }

int ControlType(const Node& n) {
  const std::wstring& c = n.class_name;
  auto ends = [&c](const wchar_t* suffix) {
    size_t k = wcslen(suffix);
    return c.size() >= k && c.compare(c.size() - k, k, suffix) == 0;
  };
  if (n.hosted != nullptr) return UIA_PaneControlTypeId;
  if (ends(L"EditText") || (n.flags & kEditable)) return UIA_EditControlTypeId;
  if (ends(L"CheckBox") || ends(L"Switch") || ends(L"ToggleButton")) return UIA_CheckBoxControlTypeId;
  if (ends(L"RadioButton")) return UIA_RadioButtonControlTypeId;
  if (ends(L"Button") || ends(L"ImageButton")) return UIA_ButtonControlTypeId;
  if (ends(L"ImageView")) return UIA_ImageControlTypeId;
  if (ends(L"ProgressBar")) return UIA_ProgressBarControlTypeId;
  if (ends(L"SeekBar")) return UIA_SliderControlTypeId;
  if (ends(L"Spinner")) return UIA_ComboBoxControlTypeId;
  if (ends(L"ListView") || ends(L"RecyclerView") || ends(L"GridView")) return UIA_ListControlTypeId;
  if (ends(L"ScrollView")) return UIA_PaneControlTypeId;
  if (ends(L"TextView")) return UIA_TextControlTypeId;
  if (n.flags & kCheckable) return UIA_CheckBoxControlTypeId;
  if (n.flags & kClickable) return UIA_ButtonControlTypeId;
  if (!n.children.empty()) return UIA_GroupControlTypeId;
  return n.name.empty() ? UIA_CustomControlTypeId : UIA_TextControlTypeId;
}

// "id\tparent\tclass\tname\tvalue\thelp\tflags\tl t r b\tchild,child\thosted\tautomationId"
Node Parse(const std::wstring& s) {
  Node n;
  std::vector<std::wstring> f = Split(s, L'\t');
  if (f.size() < 11) return n;
  n.id = ToId(f[0]);
  n.parent = ToId(f[1]);
  n.class_name = f[2];
  n.name = f[3];
  n.value = f[4];
  n.help = f[5];
  n.flags = static_cast<uint32_t>(wcstoul(f[6].c_str(), nullptr, 10));
  std::vector<std::wstring> b = Split(f[7], L' ');
  if (b.size() == 4) {
    n.bounds = {static_cast<LONG>(ToId(b[0])), static_cast<LONG>(ToId(b[1])),
                static_cast<LONG>(ToId(b[2])), static_cast<LONG>(ToId(b[3]))};
  }
  if (!f[8].empty()) {
    for (const std::wstring& c : Split(f[8], L',')) n.children.push_back(ToId(c));
  }
  n.hosted = reinterpret_cast<HWND>(static_cast<intptr_t>(ToId(f[9])));
  n.automation_id = f[10];
  n.control_type = ControlType(n);
  n.ok = true;
  return n;
}

// The root's id: UiaBridge's ROOT (AccessibilityNodeInfo.ROOT_NODE_ID).
constexpr int64_t kRoot = -1;

Node GetNode(HWND hwnd, int64_t id) {
  ScopedEnv scoped;
  JNIEnv* env = scoped.get();
  if (env == nullptr || g_bridge == nullptr) return Node();
  auto s = static_cast<jstring>(env->CallStaticObjectMethod(
      g_bridge, g_node, static_cast<jlong>(reinterpret_cast<intptr_t>(hwnd)), static_cast<jlong>(id)));
  Clear(env);
  if (s == nullptr) return Node();
  Node n = Parse(Str(env, s));
  env->DeleteLocalRef(s);
  return n;
}

int64_t NodeAt(HWND hwnd, POINT client) {
  ScopedEnv scoped;
  JNIEnv* env = scoped.get();
  if (env == nullptr) return 0;
  jlong r = env->CallStaticLongMethod(g_bridge, g_node_at,
                                      static_cast<jlong>(reinterpret_cast<intptr_t>(hwnd)),
                                      static_cast<jint>(client.x), static_cast<jint>(client.y));
  Clear(env);
  return r;
}

int64_t Focused(HWND hwnd) {
  ScopedEnv scoped;
  JNIEnv* env = scoped.get();
  if (env == nullptr) return 0;
  jlong r = env->CallStaticLongMethod(g_bridge, g_focused,
                                      static_cast<jlong>(reinterpret_cast<intptr_t>(hwnd)));
  Clear(env);
  return r;
}

enum Action : int32_t { kClick = 1, kFocus = 2, kSetText = 3, kShowOnScreen = 5 };

bool Perform(HWND hwnd, int64_t id, int32_t action, const wchar_t* text) {
  ScopedEnv scoped;
  JNIEnv* env = scoped.get();
  if (env == nullptr) return false;
  jstring jt = text != nullptr
      ? env->NewString(reinterpret_cast<const jchar*>(text), static_cast<jsize>(wcslen(text)))
      : nullptr;
  jboolean r = env->CallStaticBooleanMethod(g_bridge, g_perform,
                                            static_cast<jlong>(reinterpret_cast<intptr_t>(hwnd)),
                                            static_cast<jlong>(id), action, jt);
  Clear(env);
  if (jt != nullptr) env->DeleteLocalRef(jt);
  return r == JNI_TRUE;
}

BSTR Bstr(const std::wstring& s) { return SysAllocStringLen(s.data(), static_cast<UINT>(s.size())); }

// --- The providers ---------------------------------------------------------------

class Element final : public IRawElementProviderSimple,
                      public IRawElementProviderFragment,
                      public IRawElementProviderFragmentRoot,
                      public IInvokeProvider,
                      public IValueProvider,
                      public IToggleProvider {
 public:
  Element(HWND hwnd, int64_t id) : hwnd_(hwnd), id_(id) {}

  static Element* Make(HWND hwnd, int64_t id) { return new Element(hwnd, id); }

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (out == nullptr) return E_POINTER;
    *out = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(IRawElementProviderSimple))) {
      *out = static_cast<IRawElementProviderSimple*>(this);
    } else if (IsEqualIID(riid, __uuidof(IRawElementProviderFragment))) {
      *out = static_cast<IRawElementProviderFragment*>(this);
    } else if (IsEqualIID(riid, __uuidof(IRawElementProviderFragmentRoot)) && id_ == kRoot) {
      *out = static_cast<IRawElementProviderFragmentRoot*>(this);
    } else if (IsEqualIID(riid, __uuidof(IInvokeProvider))) {
      *out = static_cast<IInvokeProvider*>(this);
    } else if (IsEqualIID(riid, __uuidof(IValueProvider))) {
      *out = static_cast<IValueProvider*>(this);
    } else if (IsEqualIID(riid, __uuidof(IToggleProvider))) {
      *out = static_cast<IToggleProvider*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
  }

  // IRawElementProviderSimple
  HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* out) override {
    if (out == nullptr) return E_POINTER;
    *out = ProviderOptions_ServerSideProvider;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern, IUnknown** out) override {
    if (out == nullptr) return E_POINTER;
    *out = nullptr;
    Node n = Get();
    if (!n.ok) return UIA_E_ELEMENTNOTAVAILABLE;
    bool want = false;
    if (pattern == UIA_InvokePatternId) {
      want = (n.flags & (kClickable | kLongClickable)) && !(n.flags & kCheckable) &&
             n.control_type != UIA_EditControlTypeId;
    } else if (pattern == UIA_ValuePatternId) {
      want = (n.flags & kEditable) != 0;
    } else if (pattern == UIA_TogglePatternId) {
      want = (n.flags & kCheckable) != 0;
    }
    if (want) {
      *out = static_cast<IRawElementProviderSimple*>(this);
      AddRef();
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT* out) override {
    if (out == nullptr) return E_POINTER;
    VariantInit(out);
    Node n = Get();
    if (!n.ok) return UIA_E_ELEMENTNOTAVAILABLE;
    auto str = [out](const std::wstring& s) {
      out->vt = VT_BSTR;
      out->bstrVal = Bstr(s);
    };
    auto boolean = [out](bool b) {
      out->vt = VT_BOOL;
      out->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
    };
    switch (property) {
      case UIA_NamePropertyId:
        if (!n.name.empty()) str(n.name);
        break;
      case UIA_ControlTypePropertyId:
        out->vt = VT_I4;
        out->lVal = id_ == kRoot ? UIA_PaneControlTypeId : n.control_type;
        break;
      case UIA_ClassNamePropertyId:
        str(n.class_name);
        break;
      case UIA_AutomationIdPropertyId:
        if (!n.automation_id.empty()) str(n.automation_id);
        break;
      case UIA_FrameworkIdPropertyId:
        str(L"Android");
        break;
      case UIA_HelpTextPropertyId:
        if (!n.help.empty()) str(n.help);
        break;
      case UIA_IsEnabledPropertyId:
        boolean((n.flags & kEnabled) != 0);
        break;
      case UIA_IsKeyboardFocusablePropertyId:
        boolean((n.flags & kFocusable) != 0);
        break;
      case UIA_HasKeyboardFocusPropertyId:
        boolean((n.flags & kFocused) != 0);
        break;
      case UIA_IsPasswordPropertyId:
        boolean((n.flags & kPassword) != 0);
        break;
      case UIA_IsOffscreenPropertyId:
        boolean((n.flags & kVisible) == 0);
        break;
      case UIA_IsControlElementPropertyId:
      case UIA_IsContentElementPropertyId:
        boolean(true);
        break;
      case UIA_ValueValuePropertyId:
        if (n.flags & kEditable) str(n.value);
        break;
      case UIA_ValueIsReadOnlyPropertyId:
        if (n.flags & kEditable) boolean((n.flags & kEnabled) == 0);
        break;
      case UIA_ToggleToggleStatePropertyId:
        if (n.flags & kCheckable) {
          out->vt = VT_I4;
          out->lVal = (n.flags & kChecked) ? ToggleState_On : ToggleState_Off;
        }
        break;
      case UIA_HeadingLevelPropertyId:
        if (n.flags & kHeading) {
          out->vt = VT_I4;
          out->lVal = 80052;  // HeadingLevel2
        }
        break;
      case UIA_ProcessIdPropertyId:
        out->vt = VT_I4;
        out->lVal = static_cast<LONG>(GetCurrentProcessId());
        break;
      default:
        break;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** out) override {
    if (out == nullptr) return E_POINTER;
    *out = nullptr;
    if (id_ == kRoot) return UiaHostProviderFromHwnd(hwnd_, out);
    Node n = Get();
    if (n.ok && n.hosted != nullptr && IsWindow(n.hosted)) return UiaHostProviderFromHwnd(n.hosted, out);
    return S_OK;
  }

  // IRawElementProviderFragment
  HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction,
                                     IRawElementProviderFragment** out) override {
    if (out == nullptr) return E_POINTER;
    *out = nullptr;
    Node n = Get();
    if (!n.ok) return UIA_E_ELEMENTNOTAVAILABLE;
    int64_t target = 0;
    bool found = false;
    switch (direction) {
      case NavigateDirection_Parent:
        if (id_ == kRoot) return S_OK;
        target = n.parent == 0 || IsRootId(n.parent) ? kRoot : n.parent;
        found = true;
        break;
      case NavigateDirection_FirstChild:
      case NavigateDirection_LastChild:
        if (n.hosted != nullptr || n.children.empty()) return S_OK;
        target = direction == NavigateDirection_FirstChild ? n.children.front() : n.children.back();
        found = true;
        break;
      case NavigateDirection_NextSibling:
      case NavigateDirection_PreviousSibling: {
        if (id_ == kRoot) return S_OK;
        Node p = GetNode(hwnd_, n.parent == 0 || IsRootId(n.parent) ? kRoot : n.parent);
        if (!p.ok) return S_OK;
        for (size_t i = 0; i < p.children.size(); i++) {
          if (p.children[i] != n.id) continue;
          if (direction == NavigateDirection_NextSibling && i + 1 < p.children.size()) {
            target = p.children[i + 1];
            found = true;
          } else if (direction == NavigateDirection_PreviousSibling && i > 0) {
            target = p.children[i - 1];
            found = true;
          }
          break;
        }
        break;
      }
    }
    if (!found) return S_OK;
    *out = static_cast<IRawElementProviderFragment*>(Make(hwnd_, target));
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** out) override {
    if (out == nullptr) return E_POINTER;
    *out = nullptr;
    if (id_ == kRoot) return S_OK;  // the window's
    int ids[3] = {UiaAppendRuntimeId, static_cast<int>(static_cast<uint64_t>(id_) >> 32),
                  static_cast<int>(id_ & 0xffffffff)};
    SAFEARRAY* sa = SafeArrayCreateVector(VT_I4, 0, 3);
    if (sa == nullptr) return E_OUTOFMEMORY;
    for (LONG i = 0; i < 3; i++) SafeArrayPutElement(sa, &i, &ids[i]);
    *out = sa;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* out) override {
    if (out == nullptr) return E_POINTER;
    *out = {0, 0, 0, 0};
    RECT r;
    if (id_ == kRoot) {
      GetClientRect(hwnd_, &r);
    } else {
      Node n = Get();
      if (!n.ok) return UIA_E_ELEMENTNOTAVAILABLE;
      r = n.bounds;
    }
    POINT origin = {0, 0};
    ClientToScreen(hwnd_, &origin);
    out->left = r.left + origin.x;
    out->top = r.top + origin.y;
    out->width = r.right - r.left;
    out->height = r.bottom - r.top;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** out) override {
    if (out != nullptr) *out = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetFocus() override {
    Perform(hwnd_, id_, kFocus, nullptr);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** out) override {
    if (out == nullptr) return E_POINTER;
    *out = static_cast<IRawElementProviderFragmentRoot*>(Make(hwnd_, kRoot));
    return S_OK;
  }

  // IRawElementProviderFragmentRoot (the root only)
  HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y,
                                                     IRawElementProviderFragment** out) override {
    if (out == nullptr) return E_POINTER;
    POINT p = {static_cast<LONG>(x), static_cast<LONG>(y)};
    ScreenToClient(hwnd_, &p);
    int64_t id = NodeAt(hwnd_, p);
    *out = static_cast<IRawElementProviderFragment*>(Make(hwnd_, id != 0 ? id : kRoot));
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** out) override {
    if (out == nullptr) return E_POINTER;
    *out = nullptr;
    int64_t id = Focused(hwnd_);
    if (id != 0) *out = static_cast<IRawElementProviderFragment*>(Make(hwnd_, id));
    return S_OK;
  }

  // IInvokeProvider
  HRESULT STDMETHODCALLTYPE Invoke() override {
    return Perform(hwnd_, id_, kClick, nullptr) ? S_OK : UIA_E_ELEMENTNOTENABLED;
  }
  // IValueProvider
  HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) override {
    return Perform(hwnd_, id_, kSetText, value != nullptr ? value : L"") ? S_OK
                                                                        : UIA_E_ELEMENTNOTENABLED;
  }
  HRESULT STDMETHODCALLTYPE get_Value(BSTR* out) override {
    if (out == nullptr) return E_POINTER;
    Node n = Get();
    *out = Bstr(n.value);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* out) override {
    if (out == nullptr) return E_POINTER;
    Node n = Get();
    *out = !(n.flags & kEnabled);
    return S_OK;
  }
  // IToggleProvider
  HRESULT STDMETHODCALLTYPE Toggle() override {
    return Perform(hwnd_, id_, kClick, nullptr) ? S_OK : UIA_E_ELEMENTNOTENABLED;
  }
  HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* out) override {
    if (out == nullptr) return E_POINTER;
    Node n = Get();
    *out = (n.flags & kChecked) ? ToggleState_On : ToggleState_Off;
    return S_OK;
  }

 private:
  ~Element() = default;

  // The root view's own id is ROOT_NODE_ID's view: Java reports it as kRoot.
  static bool IsRootId(int64_t id) { return id == kRoot; }

  // The node, read again when 200 ms old (a UIA client asks many properties in a row).
  Node Get() {
    DWORD now = GetTickCount();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (cached_ && now - read_at_ <= 200) return node_;
    }
    // Java can raise a UIA event while answering the query. Do not hold this
    // provider's lock across that callback: UIA may query the same element again.
    Node node = GetNode(hwnd_, id_);
    std::lock_guard<std::mutex> lock(mutex_);
    node_ = std::move(node);
    cached_ = true;
    read_at_ = GetTickCount();
    return node_;
  }

  std::atomic<ULONG> refs_{1};
  HWND hwnd_;
  int64_t id_;
  std::mutex mutex_;
  bool cached_ = false;
  DWORD read_at_ = 0;
  Node node_;
};

// --- The windows ------------------------------------------------------------------

std::mutex g_windows_mutex;
std::map<HWND, WNDPROC> g_original;  // subclassed windows: their own window procedures


LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  WNDPROC original = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    auto it = g_original.find(hwnd);
    if (it != g_original.end()) original = it->second;
  }
  if (msg == WM_GETOBJECT && static_cast<LONG>(lparam) == UiaRootObjectId) {
    Element* root = Element::Make(hwnd, kRoot);
    LRESULT r = UiaReturnRawElementProvider(hwnd, wparam, lparam, root);
    root->Release();
    return r;
  }
  if (msg == WM_NCDESTROY) {
    UiaReturnRawElementProvider(hwnd, 0, 0, nullptr);
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    g_original.erase(hwnd);
  }
  return original != nullptr ? CallWindowProcW(original, hwnd, msg, wparam, lparam)
                             : DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool IsAppWindow(HWND hwnd) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId()) return false;
  wchar_t name[32];
  return GetClassNameW(hwnd, name, 32) > 0 && wcscmp(name, L"GesherWindow") == 0;
}

void Subclass(HWND hwnd) {
  if (!IsAppWindow(hwnd)) return;
  std::lock_guard<std::mutex> lock(g_windows_mutex);
  if (g_original.count(hwnd)) return;
  auto previous = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&SubclassProc)));
  if (previous != nullptr) g_original[hwnd] = previous;
}

BOOL CALLBACK EnumOne(HWND hwnd, LPARAM) {
  Subclass(hwnd);
  return TRUE;
}

void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD, HWND hwnd, LONG object, LONG child, DWORD, DWORD) {
  if (hwnd != nullptr && object == OBJID_WINDOW && child == CHILDID_SELF) Subclass(hwnd);
}

DWORD WINAPI HelperMain(void*) {
  SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, nullptr, OnWinEvent,
                  GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
  EnumWindows(EnumOne, 0);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}

// --- JNI ------------------------------------------------------------------------

std::once_flag g_start_once;

jboolean UiaBridge_nativeStart(JNIEnv*, jclass) {
  std::call_once(g_start_once, [] {
    HANDLE t = CreateThread(nullptr, 0, HelperMain, nullptr, 0, nullptr);
    if (t != nullptr) CloseHandle(t);
  });
  return JNI_TRUE;
}

jboolean UiaBridge_nativeScreenReader(JNIEnv*, jclass) {
  BOOL on = FALSE;
  SystemParametersInfoW(SPI_GETSCREENREADER, 0, &on, 0);
  return on ? JNI_TRUE : JNI_FALSE;
}

jboolean UiaBridge_nativeClientsListening(JNIEnv*, jclass) {
  return UiaClientsAreListening() ? JNI_TRUE : JNI_FALSE;
}

// An AccessibilityEvent for UIA. kind: 1 focus, 2 structure changed, 3 a
// notification (an announcement: text), 4 invoked, 5 value changed (text),
// 6 toggle state changed.
void UiaBridge_nativeRaise(JNIEnv* env, jclass, jlong hwnd_value, jlong id, jint kind, jstring text) {
  HWND hwnd = reinterpret_cast<HWND>(static_cast<intptr_t>(hwnd_value));
  if (!IsWindow(hwnd) || !UiaClientsAreListening()) return;
  Element* e = Element::Make(hwnd, id == 0 ? kRoot : id);
  std::wstring t = Str(env, text);
  switch (kind) {
    case 1:
      UiaRaiseAutomationEvent(e, UIA_AutomationFocusChangedEventId);
      break;
    case 2: {
      int ids[3] = {UiaAppendRuntimeId, 0, 0};
      UiaRaiseStructureChangedEvent(e, StructureChangeType_ChildrenInvalidated, ids, 3);
      break;
    }
    case 3: {
      BSTR b = Bstr(t), a = Bstr(L"android.announcement");
      UiaRaiseNotificationEvent(e, NotificationKind_Other, NotificationProcessing_ImportantMostRecent,
                                b, a);
      SysFreeString(b);
      SysFreeString(a);
      break;
    }
    case 4:
      UiaRaiseAutomationEvent(e, UIA_Invoke_InvokedEventId);
      break;
    case 5: {
      VARIANT before, after;
      VariantInit(&before);
      VariantInit(&after);
      after.vt = VT_BSTR;
      after.bstrVal = Bstr(t);
      UiaRaiseAutomationPropertyChangedEvent(e, UIA_ValueValuePropertyId, before, after);
      VariantClear(&after);
      break;
    }
    case 6: {
      VARIANT before, after;
      VariantInit(&before);
      VariantInit(&after);
      UiaRaiseAutomationPropertyChangedEvent(e, UIA_ToggleToggleStatePropertyId, before, after);
      break;
    }
    default:
      break;
  }
  e->Release();
}

const JNINativeMethod kMethods[] = {
    {"nativeStart", "()Z", NATIVEHELPER_SYSV_JNI_ENTRY(UiaBridge_nativeStart)},
    {"nativeScreenReader", "()Z", NATIVEHELPER_SYSV_JNI_ENTRY(UiaBridge_nativeScreenReader)},
    {"nativeClientsListening", "()Z", NATIVEHELPER_SYSV_JNI_ENTRY(UiaBridge_nativeClientsListening)},
    {"nativeRaise", "(JJILjava/lang/String;)V", NATIVEHELPER_SYSV_JNI_ENTRY(UiaBridge_nativeRaise)},
};

}  // namespace

// uia_client.cc's natives (the WebView's side).
jint RegisterUiaClientNatives(JNIEnv* env);

namespace {

jint OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  g_vm = vm;
  jclass c = env->FindClass("com/gesher/app/accessibility/UiaBridge");
  if (c == nullptr) return JNI_ERR;
  g_bridge = static_cast<jclass>(env->NewGlobalRef(c));
  g_node = env->GetStaticMethodID(c, "node", "(JJ)Ljava/lang/String;");
  g_node_at = env->GetStaticMethodID(c, "nodeAt", "(JII)J");
  g_focused = env->GetStaticMethodID(c, "focused", "(J)J");
  g_perform = env->GetStaticMethodID(c, "perform", "(JJILjava/lang/String;)Z");
  if (g_node == nullptr || g_node_at == nullptr || g_focused == nullptr || g_perform == nullptr) {
    return JNI_ERR;
  }
  if (env->RegisterNatives(c, kMethods, sizeof(kMethods) / sizeof(kMethods[0])) != JNI_OK) {
    return JNI_ERR;
  }
  env->DeleteLocalRef(c);
  if (RegisterUiaClientNatives(env) != JNI_OK) return JNI_ERR;
  return JNI_VERSION_1_6;
}

const gesher_builtin_symbol kA11y[] = {
    {"JNI_OnLoad", reinterpret_cast<void*>(&OnLoad)},
};

[[maybe_unused]] const bool registered = [] {
  gesher_register_builtin_library("libgesher_a11y.so", kA11y, sizeof(kA11y) / sizeof(kA11y[0]));
  return true;
}();

}  // namespace
