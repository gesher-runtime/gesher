// Included only in the CI fixture build, inside uia_bridge.cc's namespace.
// Dispatches the EXISTING four query wrappers on a fixture-owned real HWND's
// owner STA. No production dispatch path or JNI implementation is duplicated.
struct FixtureQuery {
  HWND hwnd = nullptr;
  WNDPROC original = nullptr;
  Epoch epoch;
  int kind = 0;
  int64_t node = kRoot;
  bool direct = false;
  HANDLE sender = nullptr;
  std::atomic<bool> entered{false}, done{false};
  std::atomic<bool> unavailable{false};
};
std::mutex g_fixture_mutex;
std::map<HWND, std::shared_ptr<FixtureQuery>> g_fixture_hooks;
std::map<uint64_t, std::shared_ptr<FixtureQuery>> g_fixture_queries;
uint64_t g_fixture_next = 1;
const UINT kFixtureMessage = RegisterWindowMessageW(L"Gesher.CI.QueryBoundary.v1");

LRESULT CALLBACK FixtureProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  std::shared_ptr<FixtureQuery> state;
  {
    std::lock_guard<std::mutex> lock(g_fixture_mutex);
    auto found = g_fixture_hooks.find(hwnd);
    if (found != g_fixture_hooks.end()) state = found->second;
  }
  if (!state) return DefWindowProcW(hwnd, msg, wparam, lparam);
  if (msg == kFixtureMessage && lparam == reinterpret_cast<LPARAM>(state.get())) {
    state->entered.store(true, std::memory_order_release);
    bool unavailable = false;
    if (state->direct) {
      // Explicit unwrapped control: the real JNI operation executes directly
      // on the owner STA, reproducing the source-level inversion. This is not
      // presented as execution of the historical binary or a qualifying fix.
      Query q;
      q.hwnd = reinterpret_cast<uintptr_t>(hwnd); q.window = state->epoch;
      q.kind = static_cast<QueryKind>(state->kind); q.id = state->node; q.token = UINT64_MAX;
      g_fixture_direct_query = &q;
      ExecuteQuery(q);
      g_fixture_direct_query = nullptr;
      unavailable = !q.okay || (q.kind != QueryKind::kNode ? q.answer == 0 : q.node.empty());
    } else {
      switch (state->kind) {
        case 0: unavailable = !GetNode(hwnd, state->node, state->epoch).ok; break;
        case 1: unavailable = NodeAt(hwnd, POINT{0, 0}, state->epoch) == 0; break;
        case 2: unavailable = Focused(hwnd, state->epoch) == 0; break;
        case 3: unavailable = !Perform(hwnd, state->node, kClick, nullptr, state->epoch); break;
      }
    }
    state->unavailable.store(unavailable, std::memory_order_release);
    state->done.store(true, std::memory_order_release);
    return 1;
  }
  if (msg == WM_NCDESTROY) {
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(state->original));
    std::lock_guard<std::mutex> lock(g_fixture_mutex);
    g_fixture_hooks.erase(hwnd);
  }
  return CallWindowProcW(state->original, hwnd, msg, wparam, lparam);
}
DWORD WINAPI FixtureSender(void* pointer) {
  std::unique_ptr<std::shared_ptr<FixtureQuery>> owner(
      static_cast<std::shared_ptr<FixtureQuery>*>(pointer));
  auto state = *owner;
  SendMessageW(state->hwnd, kFixtureMessage, 0, reinterpret_cast<LPARAM>(state.get()));
  return 0;
}
jlong QueryFixture_nativeStart(JNIEnv*, jclass, jlong value, jint kind, jlong node, jboolean direct) {
  HWND hwnd = reinterpret_cast<HWND>(static_cast<intptr_t>(value));
  if (!IsAppWindow(hwnd) || kind < 0 || kind > 3 || kFixtureMessage == 0) return 0;
  Subclass(hwnd);  // preserve UIA's original chain, including NCDESTROY cleanup
  Epoch epoch = EpochOf(hwnd);
  if (!epoch || !epoch->alive.load()) return 0;
  auto state = std::make_shared<FixtureQuery>();
  state->hwnd = hwnd; state->epoch = epoch; state->kind = kind; state->node = node; state->direct = direct == JNI_TRUE;
  uint64_t id;
  {
    std::lock_guard<std::mutex> lock(g_fixture_mutex);
    if (g_fixture_hooks.count(hwnd) || g_fixture_next == 0) return 0;
    id = g_fixture_next++;
    state->original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(FixtureProc)));
    if (!state->original) return 0;
    g_fixture_hooks[hwnd] = state;
    g_fixture_queries[id] = state;
  }
  auto* owner = new std::shared_ptr<FixtureQuery>(state);
  state->sender = CreateThread(nullptr, 0, FixtureSender, owner, 0, nullptr);
  if (!state->sender) {
    delete owner;
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(state->original));
    std::lock_guard<std::mutex> lock(g_fixture_mutex);
    g_fixture_hooks.erase(hwnd); g_fixture_queries.erase(id);
    return 0;
  }
  return static_cast<jlong>(id);
}
jlong QueryFixture_nativeState(JNIEnv*, jclass, jlong id) {
  std::shared_ptr<FixtureQuery> state;
  {
    std::lock_guard<std::mutex> lock(g_fixture_mutex);
    auto found = g_fixture_queries.find(static_cast<uint64_t>(id));
    if (found == g_fixture_queries.end()) return -1;
    state = found->second;
  }
  return (state->entered.load() ? 1 : 0) | (state->done.load() ? 2 : 0) |
         (state->unavailable.load() ? 4 : 0);
}
jlong QueryFixture_nativeGeneration(JNIEnv*, jclass, jlong id) {
  std::lock_guard<std::mutex> lock(g_fixture_mutex);
  auto found = g_fixture_queries.find(static_cast<uint64_t>(id));
  return found == g_fixture_queries.end() ? 0 : static_cast<jlong>(found->second->epoch->generation);
}
jboolean QueryFixture_nativeJoin(JNIEnv*, jclass, jlong id) {
  std::shared_ptr<FixtureQuery> state;
  {
    std::lock_guard<std::mutex> lock(g_fixture_mutex);
    auto found = g_fixture_queries.find(static_cast<uint64_t>(id));
    if (found == g_fixture_queries.end()) return JNI_FALSE;
    state = found->second;
  }
  if (WaitForSingleObject(state->sender, 3000) != WAIT_OBJECT_0) return JNI_FALSE;
  DWORD code = 1;
  bool okay = GetExitCodeThread(state->sender, &code) && code == 0;
  CloseHandle(state->sender);
  std::lock_guard<std::mutex> lock(g_fixture_mutex);
  // A completed request on a LIVE mapped window must release only its own
  // hook, so the next real query can install a fresh payload on that HWND.
  // NCDESTROY may already have erased it; never touch a reused HWND's hook.
  auto hook = g_fixture_hooks.find(state->hwnd);
  if (hook != g_fixture_hooks.end() && hook->second == state) {
    if (IsWindow(state->hwnd)) {
      if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(state->hwnd, GWLP_WNDPROC)) != FixtureProc ||
          SetWindowLongPtrW(state->hwnd, GWLP_WNDPROC,
                           reinterpret_cast<LONG_PTR>(state->original)) == 0) okay = false;
    }
    g_fixture_hooks.erase(hook);
  }
  g_fixture_queries.erase(static_cast<uint64_t>(id));
  return okay ? JNI_TRUE : JNI_FALSE;
}
jint QueryFixture_nativeActive(JNIEnv*, jclass) { return static_cast<jint>(g_queries->ActiveCount()); }
jint QueryFixture_nativeAttached(JNIEnv*, jclass) { return static_cast<jint>(g_attached_queries.load()); }
jint QueryFixture_nativeShutdowns(JNIEnv*, jclass) { return static_cast<jint>(g_fixture_shutdowns.load()); }
jboolean QueryFixture_nativeStalePerform(JNIEnv*, jclass, jlong id) {
  std::shared_ptr<FixtureQuery> state;
  {
    std::lock_guard<std::mutex> lock(g_fixture_mutex);
    auto found = g_fixture_queries.find(static_cast<uint64_t>(id));
    if (found == g_fixture_queries.end()) return JNI_TRUE; // fail the rejection assertion
    state = found->second;
  }
  return Perform(state->hwnd, state->node, kClick, nullptr, state->epoch) ? JNI_TRUE : JNI_FALSE;
}
jboolean RegisterQueryFixture(JNIEnv* env, jclass, jclass type) {
  if (!type) return JNI_FALSE;
  const JNINativeMethod methods[] = {
    {"nativeStart", "(JIJZ)J", NATIVEHELPER_SYSV_JNI_ENTRY(QueryFixture_nativeStart)},
    {"nativeState", "(J)J", NATIVEHELPER_SYSV_JNI_ENTRY(QueryFixture_nativeState)},
    {"nativeGeneration", "(J)J", NATIVEHELPER_SYSV_JNI_ENTRY(QueryFixture_nativeGeneration)},
    {"nativeJoin", "(J)Z", NATIVEHELPER_SYSV_JNI_ENTRY(QueryFixture_nativeJoin)},
    {"nativeActive", "()I", NATIVEHELPER_SYSV_JNI_ENTRY(QueryFixture_nativeActive)},
    {"nativeAttached", "()I", NATIVEHELPER_SYSV_JNI_ENTRY(QueryFixture_nativeAttached)},
    {"nativeStalePerform", "(J)Z", NATIVEHELPER_SYSV_JNI_ENTRY(QueryFixture_nativeStalePerform)},
    {"nativeShutdowns", "()I", NATIVEHELPER_SYSV_JNI_ENTRY(QueryFixture_nativeShutdowns)},
  };
  bool okay = env->RegisterNatives(type, methods, sizeof(methods) / sizeof(methods[0])) == JNI_OK;
  return okay ? JNI_TRUE : JNI_FALSE;
}
