// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Included inside webview_host.cc's private STA namespace, after its helpers.
// No application WebView is navigated or borrowed by this operation.

struct OriginStorageDelete;
std::shared_ptr<OriginStorageDelete> g_origin_delete;
std::deque<std::shared_ptr<OriginStorageDelete>> g_origin_delete_queue;
void NextOriginStorageDelete();
void CALLBACK OriginStorageDeleteTimer(HWND, UINT, UINT_PTR, DWORD);

struct OriginStorageDelete : std::enable_shared_from_this<OriginStorageDelete> {
  using Json = cdp_json::Json;
  using Key = storage_key_selection::Key;
  using Result = std::function<void(const Json&)>;
  static constexpr UINT_PTR kTimer = 0x4753;
  static constexpr ULONGLONG kOperationMs = 120000;
  static constexpr ULONGLONG kStepMs = 20000;
  static constexpr size_t kPageSize = 128;
  static constexpr size_t kJsonLimit = 1024 * 1024;

  struct View {
    Com<ICoreWebView2Controller> controller;
    Com<ICoreWebView2> core;
    EventRegistrationToken resources{};
    bool resource_handler = false;
  };
  struct PendingCall { Result then; };
  struct Creation {
    GlobalWork lease;
    std::function<void(HRESULT, ICoreWebView2Controller*)> then;
    bool released_queue = false;
  };

  GlobalWork operation;
  int64_t callback_id;
  std::wstring requested_origin;
  Com<ICoreWebView2Environment> environment;
  View inventory, resolver;
  std::wstring profile_name, profile_path;
  std::shared_ptr<PendingCall> pending;
  std::shared_ptr<Creation> creation;
  Com<ICoreWebView2> navigation_core;
  EventRegistrationToken navigating{}, navigated{};
  bool has_navigating = false, has_navigated = false, has_navigation_id = false;
  UINT64 navigation_id = 0;
  std::wstring navigation_url;
  std::function<void()> navigation_then;
  ULONGLONG deadline = 0, step_deadline = 0;
  bool finished = false, timer = false;
  size_t inventory_total = 0;
  std::vector<Key> keys;
  std::set<std::wstring> seen_keys;
  std::map<std::wstring, std::wstring> sites;
  std::deque<std::wstring> unresolved;
  std::vector<std::wstring> deletions;
  size_t delete_at = 0;
  std::wstring resolving, resolving_scheme, resolver_url, child_url;
  unsigned sentinel = 0;

  OriginStorageDelete(GlobalWork work, std::wstring origin, int64_t id)
      : operation(std::move(work)), callback_id(id), requested_origin(std::move(origin)) {}

  void Arm() { step_deadline = std::min(deadline, GetTickCount64() + kStepMs); }

  bool Check(HRESULT hr, const wchar_t* where) {
    if (finished) return false;
    if (FAILED(hr)) { Finish(hr, where); return false; }
    return true;
  }

  static bool Number(const Json* value, size_t* out) {
    if (!value || value->kind != L'#' || value->text.empty()) return false;
    size_t result = 0;
    for (wchar_t c : value->text) {
      if (c < L'0' || c > L'9' || result > (SIZE_MAX - size_t(c - L'0')) / 10)
        return false;
      result = result * 10 + size_t(c - L'0');
    }
    *out = result;
    return true;
  }

  static std::wstring NodeId(const Json* value) {
    size_t number = 0;
    return Number(value, &number) && number > 0 && number <= INT32_MAX
        ? value->text : std::wstring();
  }

  HRESULT DetachNavigation() {
    HRESULT result = S_OK;
    if (navigation_core) {
      if (has_navigating) result = navigation_core->remove_NavigationStarting(navigating);
      if (has_navigated) {
        const HRESULT hr = navigation_core->remove_NavigationCompleted(navigated);
        if (SUCCEEDED(result)) result = hr;
      }
    }
    has_navigating = has_navigated = has_navigation_id = false;
    navigation_core.Reset();
    navigation_then = {};
    return result;
  }

  HRESULT Close(View* view) {
    HRESULT result = S_OK;
    if (view->resource_handler && view->core) {
      result = view->core->remove_WebResourceRequested(view->resources);
      view->resource_handler = false;
    }
    if (view->controller) {
      HRESULT hr = view->controller->Close();
      if (SUCCEEDED(result)) result = hr;
    }
    view->core.Reset();
    view->controller.Reset();
    return result;
  }

  void Finish(HRESULT hr, const std::wstring& where) {
    if (finished) return;
    auto self = shared_from_this();
    finished = true;
    if (timer) { KillTimer(g_task_window, kTimer); timer = false; }
    if (pending) pending->then = {};
    pending.reset();
    // An in-flight SDK creation retains its separate lease until its callback
    // can close a late controller. A merely queued creation has no such lease.
    if (creation) creation->then = {};
    creation.reset();
    HRESULT navigation_closed = DetachNavigation();
    HRESULT private_closed = Close(&resolver);
    HRESULT ordinary_closed = Close(&inventory);
    environment.Reset();
    if (SUCCEEDED(hr) && FAILED(navigation_closed)) hr = navigation_closed;
    if (SUCCEEDED(hr) && FAILED(private_closed)) hr = private_closed;
    if (SUCCEEDED(hr) && FAILED(ordinary_closed)) hr = ordinary_closed;
    GlobalWork work = std::move(operation);
    ReportGlobal(kGlobalDone, callback_id, static_cast<int32_t>(hr),
                 SUCCEEDED(hr) ? L"" : L"WebStorage origin deletion: " + where);
    if (g_origin_delete.get() == this) g_origin_delete.reset();
    NextOriginStorageDelete();
  }

  void Begin() {
    deadline = GetTickCount64() + kOperationMs;
    Arm();
    timer = SetTimer(g_task_window, kTimer, 250, OriginStorageDeleteTimer) != 0;
    if (!timer) {
      const DWORD error = GetLastError();
      Finish(error ? HRESULT_FROM_WIN32(error) : E_FAIL, L"deadline timer"); return;
    }
    // WithEnvironment owns this continuation only until environment completion.
    // Retain the lease independently if the operation times out during creation.
    GlobalWork lease = operation;
    std::weak_ptr<OriginStorageDelete> weak = shared_from_this();
    WithEnvironment([weak, lease](ICoreWebView2Environment* env, HRESULT hr) {
      auto self = weak.lock();
      if (!self || self->finished) return;
      if (!self->Check(hr, L"environment") || !env) {
        if (!self->finished) self->Finish(E_FAIL, L"missing environment");
        return;
      }
      self->environment = Com<ICoreWebView2Environment>(env);
      self->CreateController(false);
    });
  }

  void CreateController(bool in_private) {
    auto ticket = std::make_shared<Creation>();
    creation = ticket;
    auto self = shared_from_this();
    ticket->then = [self, in_private](HRESULT hr, ICoreWebView2Controller* controller) {
      self->creation.reset();
      if (!self->Check(hr, L"controller creation") || !controller) {
        if (controller) controller->Close();
        if (!self->finished) self->Finish(E_FAIL, L"missing controller");
        return;
      }
      View& view = in_private ? self->resolver : self->inventory;
      view.controller = Com<ICoreWebView2Controller>(controller);
      if (!self->Check(controller->put_IsVisible(FALSE), L"hide controller") ||
          !self->Check(controller->get_CoreWebView2(view.core.Put()), L"controller core") ||
          !view.core) {
        if (!self->finished) self->Finish(E_FAIL, L"missing controller core");
        return;
      }
      if (!self->VerifyProfile(view, in_private)) return;
      if (in_private) self->ConfigureResolver();
      else self->InitializeInventory();
    };
    std::weak_ptr<OriginStorageDelete> weak = self;
    g_controller_queue.push_back([weak, ticket, in_private] {
      auto self = weak.lock();
      if (!self || self->finished || !ticket->then) { ControllerDone(); return; }
      self->Arm();
      ticket->lease = self->operation;
      auto complete = [ticket](HRESULT hr, ICoreWebView2Controller* controller) {
        GlobalWork lease = std::move(ticket->lease);
        auto then = std::move(ticket->then);
        if (then) then(hr, controller);
        else if (controller) controller->Close();
        if (!ticket->released_queue) { ticket->released_queue = true; ControllerDone(); }
      };
      auto callback = GESHER_CALLBACK(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
          [complete](HRESULT hr, ICoreWebView2Controller* controller) -> HRESULT {
            complete(hr, controller); return S_OK;
          });
      HRESULT hr = E_NOINTERFACE;
      if (in_private) {
        Com<ICoreWebView2Environment10> env10;
        Com<ICoreWebView2ControllerOptions> options;
        if (self->environment.As(IID_ICoreWebView2Environment10, &env10)) {
          hr = env10->CreateCoreWebView2ControllerOptions(options.Put());
          if (SUCCEEDED(hr) && !options) hr = E_FAIL;
          if (SUCCEEDED(hr)) hr = options->put_ProfileName(self->profile_name.c_str());
          if (SUCCEEDED(hr)) hr = options->put_IsInPrivateModeEnabled(TRUE);
          if (SUCCEEDED(hr)) hr = env10->CreateCoreWebView2ControllerWithOptions(
              g_parking, options.get(), callback.get());
        }
      } else {
        hr = self->environment->CreateCoreWebView2Controller(g_parking, callback.get());
      }
      if (FAILED(hr)) complete(hr, nullptr);
    });
    NextController();
  }

  bool VerifyProfile(const View& view, bool in_private) {
    Com<ICoreWebView2_13> core13;
    Com<ICoreWebView2Profile> profile;
    CoString name, path;
    BOOL actual_private = FALSE;
    if (!view.core.As(IID_ICoreWebView2_13, &core13) ||
        FAILED(core13->get_Profile(profile.Put())) || !profile ||
        FAILED(profile->get_ProfileName(name.Put())) ||
        FAILED(profile->get_ProfilePath(path.Put())) ||
        FAILED(profile->get_IsInPrivateModeEnabled(&actual_private)) ||
        actual_private != (in_private ? TRUE : FALSE) || name.str().empty() || path.str().empty()) {
      Finish(E_FAIL, L"actual profile identity"); return false;
    }
    if (!in_private) {
      profile_name = name.str(); profile_path = path.str();
    } else if (CompareStringOrdinal(profile_name.c_str(), -1, name.str().c_str(), -1, TRUE) != CSTR_EQUAL ||
               CompareStringOrdinal(profile_path.c_str(), -1, path.str().c_str(), -1, TRUE) != CSTR_EQUAL) {
      Finish(E_FAIL, L"resolver profile isolation"); return false;
    }
    return true;
  }

  void Call(const View& view, const wchar_t* method, const std::wstring& params, Result then) {
    if (finished) return;
    Arm();
    auto call = std::make_shared<PendingCall>();
    call->then = std::move(then);
    pending = call;
    std::weak_ptr<OriginStorageDelete> weak = shared_from_this();
    auto complete = [weak, call, method = std::wstring(method)](HRESULT hr, LPCWSTR json) {
      auto self = weak.lock();
      auto then = std::move(call->then);
      if (!self || self->finished || !then) return;
      if (self->pending == call) self->pending.reset();
      if (!self->Check(hr, method.c_str())) return;
      size_t length = 0;
      while (json && length <= kJsonLimit && json[length]) ++length;
      Json result;
      if (!json || !cdp_json::JsonReader(std::wstring_view(json, length), kJsonLimit, 16384).Read(&result) ||
          result.kind != L'{' || result.fields.contains(L"error") || result.fields.contains(L"code")) {
        self->Finish(E_FAIL, L"malformed DevTools result"); return;
      }
      then(result);
    };
    Com<ICoreWebView2> retained(view.core);
    HRESULT hr = retained->CallDevToolsProtocolMethod(method, params.c_str(),
        GESHER_CALLBACK(ICoreWebView2CallDevToolsProtocolMethodCompletedHandler,
            [complete](HRESULT result, LPCWSTR json) -> HRESULT {
              complete(result, json); return S_OK;
            }).get());
    if (FAILED(hr)) complete(hr, nullptr);
  }

  void Evaluate(const View& view, const std::wstring& script, Result then) {
    auto self = shared_from_this();
    Call(view, L"Runtime.evaluate", L"{\"expression\":" + cdp_json::Quote(script) +
        L",\"awaitPromise\":true,\"returnByValue\":true}",
        [self, then = std::move(then)](const Json& result) {
          const Json* remote = result.Get(L"result", L'{');
          const Json* value = remote ? remote->Get(L"value", L'{') : nullptr;
          if (result.fields.contains(L"exceptionDetails") || !value) {
            self->Finish(E_FAIL, L"browser storage operation rejected"); return;
          }
          if (value->Get(L"unavailable", L'"')) {
            self->Finish(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), L"browser storage interface unavailable");
            return;
          }
          then(*value);
        });
  }

  void Navigate(const View& view, std::wstring url, std::function<void()> then) {
    if (finished) return;
    Arm();
    if (!Check(DetachNavigation(), L"previous navigation subscriptions")) return;
    navigation_core = view.core;
    navigation_url = std::move(url);
    navigation_then = std::move(then);
    std::weak_ptr<OriginStorageDelete> weak = shared_from_this();
    HRESULT hr = navigation_core->add_NavigationStarting(
        GESHER_CALLBACK(ICoreWebView2NavigationStartingEventHandler,
            [weak](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
              auto self = weak.lock(); if (!self || self->finished) return S_OK;
              CoString uri;
              UINT64 id = 0;
              if (!args || FAILED(args->get_Uri(uri.Put())) || uri.str() != self->navigation_url ||
                  FAILED(args->get_NavigationId(&id))) {
                if (args) args->put_Cancel(TRUE);
                self->Finish(E_FAIL, L"unexpected storage navigation"); return S_OK;
              }
              self->navigation_id = id; self->has_navigation_id = true;
              return S_OK;
            }).get(), &navigating);
    has_navigating = SUCCEEDED(hr);
    if (!Check(hr, L"navigation guard")) return;
    hr = navigation_core->add_NavigationCompleted(
        GESHER_CALLBACK(ICoreWebView2NavigationCompletedEventHandler,
            [weak](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
              auto self = weak.lock(); if (!self || self->finished) return S_OK;
              UINT64 id = 0; BOOL success = FALSE; CoString source;
              if (!args || FAILED(args->get_NavigationId(&id))) {
                self->Finish(E_FAIL, L"navigation identity"); return S_OK;
              }
              if (!self->has_navigation_id || id != self->navigation_id) return S_OK;
              if (FAILED(args->get_IsSuccess(&success)) || !success ||
                  FAILED(self->navigation_core->get_Source(source.Put())) || source.str() != self->navigation_url) {
                self->Finish(E_FAIL, L"storage navigation did not complete"); return S_OK;
              }
              auto then = std::move(self->navigation_then);
              if (self->Check(self->DetachNavigation(), L"navigation subscription cleanup") && then) then();
              return S_OK;
            }).get(), &navigated);
    has_navigated = SUCCEEDED(hr);
    if (Check(hr, L"navigation completion")) {
      Com<ICoreWebView2> retained(navigation_core);
      Check(retained->Navigate(navigation_url.c_str()), L"navigate");
    }
  }

  void InitializeInventory() {
    auto self = shared_from_this();
    Navigate(inventory, L"edge://indexeddb-internals/", [self] {
      self->Evaluate(self->inventory, storage_registry_scripts::kInitializeIndexedDb, [self](const Json& value) {
        if (!value.Get(L"initialized", L't')) {
          self->Finish(E_FAIL, L"IndexedDB disk discovery"); return;
        }
        self->Navigate(self->inventory, L"edge://quota-internals/", [self] { self->Snapshot(); });
      });
    });
  }

  void Snapshot() {
    auto self = shared_from_this();
    const std::wstring script = L"(async()=>{const result=await " +
        std::wstring(storage_registry_scripts::kReadRegistry) + LR"JS(;
      if (result.unavailable) return result;
      if (!Array.isArray(result.entries)) throw Error('missing live storage keys');
      const keys = new Set();
      for (const entry of result.entries) {
        if (typeof entry.storageKey !== 'string' || !entry.storageKey.length || entry.storageKey.length > 4096)
          throw Error('invalid live storage key');
        keys.add(entry.storageKey);
      }
      globalThis.__gesherOriginDeleteKeys = [...keys].sort();
      return {total: globalThis.__gesherOriginDeleteKeys.length};
    })())JS";
    Evaluate(inventory, script, [self](const Json& value) {
      if (!Number(value.Get(L"total", L'#'), &self->inventory_total)) {
        self->Finish(E_FAIL, L"inventory size"); return;
      }
      self->ReadPage(0);
    });
  }

  void ReadPage(size_t offset) {
    auto self = shared_from_this();
    if (offset == inventory_total) { InventoryReady(); return; }
    const std::wstring number = std::to_wstring(offset);
    Evaluate(inventory, L"(()=>{const keys=globalThis.__gesherOriginDeleteKeys;"
        L"if(!Array.isArray(keys))throw Error('lost storage snapshot');return {offset:" + number +
        L",total:keys.length,keys:keys.slice(" + number + L"," + number + L"+128)}})()",
        [self, offset](const Json& value) {
          size_t actual_offset = 0, total = 0;
          const Json* page = value.Get(L"keys", L'[');
          if (!Number(value.Get(L"offset", L'#'), &actual_offset) || actual_offset != offset ||
              !Number(value.Get(L"total", L'#'), &total) || total != self->inventory_total || !page ||
              page->items.empty() || page->items.size() > kPageSize || page->items.size() > total - offset) {
            self->Finish(E_FAIL, L"incomplete storage inventory page"); return;
          }
          for (const Json& item : page->items) {
            Key key;
            if (item.kind != L'"' || !storage_key_selection::Parse(item.text, &key) ||
                !self->seen_keys.insert(item.text).second) {
              self->Finish(E_FAIL, L"unknown or duplicate storage-key serialization"); return;
            }
            self->keys.push_back(std::move(key));
          }
          self->ReadPage(offset + page->items.size());
        });
  }

  void InventoryReady() {
    const bool partitioned = std::any_of(keys.begin(), keys.end(), [](const Key& key) {
      return key.kind == storage_key_selection::Kind::kExplicitSite ||
             key.kind == storage_key_selection::Kind::kImplicitSite;
    });
    if (partitioned) { CreateController(true); return; }
    // No third-party key exists in this snapshot. An unpartitioned runtime
    // needs no frame/site resolver, including when its inventory is empty.
    auto self = shared_from_this();
    Evaluate(inventory, L"(()=>{const u=new URL(" + cdp_json::Quote(requested_origin) +
        L");if(!['http:','https:'].includes(u.protocol))throw Error('unsupported origin');"
        L"return {origin:u.origin+'/'}})()", [self](const Json& value) {
          const Json* origin = value.Get(L"origin", L'"');
          if (!origin || origin->text.empty()) { self->Finish(E_FAIL, L"runtime origin canonicalization"); return; }
          self->requested_origin = origin->text;
          self->deletions.push_back(origin->text);
          self->DeleteNext();
        });
  }

  HRESULT Respond(ICoreWebView2WebResourceRequestedEventArgs* args) {
    Com<ICoreWebView2WebResourceRequest> request;
    CoString uri, method;
    bool readable = args && SUCCEEDED(args->get_Request(request.Put())) && request &&
        SUCCEEDED(request->get_Uri(uri.Put())) && SUCCEEDED(request->get_Method(method.Put()));
    const bool top = readable && method.str() == L"GET" && uri.str() == resolver_url;
    const bool child = readable && method.str() == L"GET" && uri.str() == child_url;
    std::string body = "<!doctype html><meta charset=utf-8>";
    if (top) {
      const std::string child_ascii(child_url.begin(), child_url.end());
      body += "<iframe id=gesher-storage-site-child src=\"" + child_ascii + "\"></iframe>";
    }
    if (!top && !child) body.clear();
    SharedBody bytes = std::make_shared<const std::vector<uint8_t>>(body.begin(), body.end());
    Com<IStream> stream;
    HRESULT hr = OwnedStream::Create(std::move(bytes), stream.Put());
    Com<ICoreWebView2WebResourceResponse> response;
    const std::wstring headers = L"Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
        L"Content-Security-Policy: default-src 'none'; frame-src " + child_url +
        L"; base-uri 'none'; form-action 'none'\r\nX-Content-Type-Options: nosniff\r\n";
    if (SUCCEEDED(hr)) hr = environment->CreateWebResourceResponse(stream.get(), top || child ? 200 : 404,
        top || child ? L"OK" : L"Not Found", headers.c_str(), response.Put());
    if (SUCCEEDED(hr) && (!response || !args)) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = args->put_Response(response.get());
    if (!readable && SUCCEEDED(hr)) hr = E_FAIL;
    return hr;
  }

  void ConfigureResolver() {
    std::weak_ptr<OriginStorageDelete> weak = shared_from_this();
    HRESULT hr = resolver.core->add_WebResourceRequested(
        GESHER_CALLBACK(ICoreWebView2WebResourceRequestedEventHandler,
            [weak](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
              auto self = weak.lock(); if (!self || self->finished) return S_OK;
              self->Check(self->Respond(args), L"private local response");
              return S_OK;
            }).get(), &resolver.resources);
    resolver.resource_handler = SUCCEEDED(hr);
    if (!Check(hr, L"private resource responder") ||
        !Check(resolver.core->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL),
               L"private resource filter")) return;
    ConfigureResolverStep(0);
  }

  void ConfigureResolverStep(unsigned step) {
    static const wchar_t* methods[] = {L"Network.enable", L"Network.setCacheDisabled",
        L"Network.setBypassServiceWorker", L"Network.emulateNetworkConditions"};
    static const wchar_t* parameters[] = {L"{}", L"{\"cacheDisabled\":true}", L"{\"bypass\":true}",
        L"{\"offline\":true,\"latency\":0,\"downloadThroughput\":-1,\"uploadThroughput\":-1}"};
    if (step == 4) { Resolve(requested_origin); return; }
    auto self = shared_from_this();
    Call(resolver, methods[step], parameters[step], [self, step](const Json&) { self->ConfigureResolverStep(step + 1); });
  }

  void Resolve(const std::wstring& origin) {
    auto self = shared_from_this();
    resolving = origin;
    sentinel = 0;
    // URL canonicalization runs only in the owned built-in document. All host
    // derivation navigations use HTTPS, avoiding an inherited HTTP HSTS upgrade.
    // SchemefulSite uses that same registrable host with the original scheme;
    // its port is never part of the site. No suffix list is copied or guessed.
    Evaluate(inventory, L"(()=>{const u=new URL(" + cdp_json::Quote(origin) +
        L");if(!['http:','https:'].includes(u.protocol))throw Error('unsupported origin');"
        L"const origin=u.origin+'/';const scheme=u.protocol;u.protocol='https:';"
        L"return {origin,scheme,resolver:u.origin+'/.gesher-storage/site'}})()",
        [self](const Json& value) {
          const Json* origin = value.Get(L"origin", L'"');
          const Json* scheme = value.Get(L"scheme", L'"');
          const Json* url = value.Get(L"resolver", L'"');
          if (!origin || !scheme || !url || (scheme->text != L"http:" && scheme->text != L"https:") ||
              !url->text.starts_with(L"https://") || !url->text.ends_with(L"/.gesher-storage/site")) {
            self->Finish(E_FAIL, L"runtime origin canonicalization"); return;
          }
          if (self->sites.empty()) {
            self->requested_origin = origin->text;
            self->resolving = origin->text;
          } else if (origin->text != self->resolving) {
            self->Finish(E_FAIL, L"noncanonical observed key origin"); return;
          }
          self->resolving_scheme = scheme->text;
          self->resolver_url = url->text;
          self->NavigateResolver();
        });
  }

  void NavigateResolver() {
    child_url = sentinel == 0 ? L"https://gesher-storage-resolver.invalid/site" : L"https://gesher-storage-resolver.test/site";
    auto self = shared_from_this();
    Navigate(resolver, resolver_url, [self] { self->CaptureSite(); });
  }

  bool ResolverDocument() {
    CoString source;
    if (FAILED(resolver.core->get_Source(source.Put())) || source.str() != resolver_url) {
      Finish(E_FAIL, L"resolver document changed"); return false;
    }
    return true;
  }

  void CaptureSite() {
    if (!ResolverDocument()) return;
    auto self = shared_from_this();
    Call(resolver, L"DOM.getDocument", L"{\"depth\":0}", [self](const Json& document) {
      if (!self->ResolverDocument()) return;
      const Json* root = document.Get(L"root", L'{');
      const std::wstring root_id = NodeId(root ? root->Get(L"nodeId", L'#') : nullptr);
      if (root_id.empty()) { self->Finish(E_FAIL, L"resolver DOM document"); return; }
      self->Call(self->resolver, L"DOM.querySelectorAll", L"{\"nodeId\":" + root_id +
          L",\"selector\":\"iframe#gesher-storage-site-child\"}", [self](const Json& selected) {
        if (!self->ResolverDocument()) return;
        const Json* nodes = selected.Get(L"nodeIds", L'[');
        if (!nodes || nodes->items.size() != 1) { self->Finish(E_FAIL, L"resolver iframe owner"); return; }
        const std::wstring node = NodeId(&nodes->items[0]);
        if (node.empty()) { self->Finish(E_FAIL, L"resolver iframe node ID"); return; }
        self->Call(self->resolver, L"DOM.describeNode", L"{\"nodeId\":" + node + L",\"depth\":0}",
            [self, node](const Json& described) { self->ReadFrameKey(described, node); });
      });
    });
  }

  void ReadFrameKey(const Json& described, const std::wstring& node_id) {
    if (!ResolverDocument()) return;
    const Json* node = described.Get(L"node", L'{');
    const Json* name = node ? node->Get(L"nodeName", L'"') : nullptr;
    const Json* type = node ? node->Get(L"nodeType", L'#') : nullptr;
    const Json* frame = node ? node->Get(L"frameId", L'"') : nullptr;
    const Json* attrs = node ? node->Get(L"attributes", L'[') : nullptr;
    std::map<std::wstring, std::wstring> attributes;
    if (attrs && attrs->items.size() % 2 == 0) {
      for (size_t i = 0; i < attrs->items.size(); i += 2) {
        const auto& key = attrs->items[i]; const auto& value = attrs->items[i + 1];
        if (key.kind != L'"' || value.kind != L'"' || !attributes.emplace(key.text, value.text).second) {
          Finish(E_FAIL, L"resolver iframe attributes"); return;
        }
      }
    }
    if (!node || NodeId(node->Get(L"nodeId", L'#')) != node_id || !name || name->text != L"IFRAME" ||
        !type || type->text != L"1" || !frame || frame->text.empty() || frame->text.size() > 128 ||
        !attrs || attrs->items.size() % 2 != 0 || attributes[L"id"] != L"gesher-storage-site-child" ||
        attributes[L"src"] != child_url) {
      Finish(E_FAIL, L"resolver iframe identity"); return;
    }
    auto self = shared_from_this();
    Call(resolver, L"Storage.getStorageKeyForFrame", L"{\"frameId\":" + cdp_json::Quote(frame->text) + L"}",
        [self](const Json& result) {
          if (!self->ResolverDocument()) return;
          const Json* value = result.Get(L"storageKey", L'"');
          Key key;
          const std::wstring child_origin = self->child_url.substr(0, self->child_url.size() - 4);
          if (!value || !storage_key_selection::Parse(value->text, &key) || key.origin != child_origin) {
            self->Finish(E_FAIL, L"resolver actual frame storage key"); return;
          }
          if (key.kind == storage_key_selection::Kind::kFirstParty && self->sentinel == 0) {
            ++self->sentinel; self->NavigateResolver(); return;
          }
          if (key.kind != storage_key_selection::Kind::kExplicitSite || !key.site.starts_with(L"https://")) {
            self->Finish(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), L"runtime schemeful-site derivation"); return;
          }
          self->sites.emplace(self->resolving, self->resolving_scheme + key.site.substr(6));
          self->ResolveNext();
        });
  }

  void ResolveNext() {
    if (sites.size() == 1 && unresolved.empty()) {
      std::set<std::wstring> origins;
      for (const auto& key : keys) {
        if (key.kind == storage_key_selection::Kind::kImplicitSite &&
            storage_key_selection::Scheme(key.origin) == storage_key_selection::Scheme(requested_origin) &&
            !sites.contains(key.origin)) origins.insert(key.origin);
      }
      unresolved.assign(origins.begin(), origins.end());
    }
    if (!unresolved.empty()) {
      std::wstring origin = std::move(unresolved.front()); unresolved.pop_front();
      Resolve(origin); return;
    }
    std::set<std::wstring> selected;
    if (!storage_key_selection::Select(keys, requested_origin, sites, &selected)) {
      Finish(E_FAIL, L"incomplete storage-key selection"); return;
    }
    // The requested first-party origin is also cleared when no quota bucket
    // existed in the snapshot. This is the explicit operation the caller asked
    // for, not an invented inventory entry or a synthesized partitioned key.
    selected.insert(requested_origin);
    deletions.assign(selected.begin(), selected.end());
    DeleteNext();
  }

  void DeleteNext() {
    if (delete_at == deletions.size()) { Finish(S_OK, L""); return; }
    auto self = shared_from_this();
    Call(inventory, L"Storage.clearDataForStorageKey", L"{\"storageKey\":" +
        cdp_json::Quote(deletions[delete_at]) + L",\"storageTypes\":\"file_systems,indexeddb\"}",
        [self](const Json& result) {
          if (!result.fields.empty()) { self->Finish(E_FAIL, L"unexpected deletion acknowledgment"); return; }
          ++self->delete_at; self->DeleteNext();
        });
  }
};

void NextOriginStorageDelete() {
  if (g_origin_delete || g_origin_delete_queue.empty()) return;
  g_origin_delete = std::move(g_origin_delete_queue.front());
  g_origin_delete_queue.pop_front();
  g_origin_delete->Begin();
}

void CALLBACK OriginStorageDeleteTimer(HWND, UINT, UINT_PTR, DWORD) {
  auto operation = g_origin_delete;
  if (operation && !operation->finished &&
      (GetTickCount64() >= operation->deadline || GetTickCount64() >= operation->step_deadline)) {
    operation->Finish(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"operation deadline");
  }
}

void QueueOriginStorageDelete(const GlobalWork& operation, std::wstring origin, int64_t id) {
  g_origin_delete_queue.push_back(std::make_shared<OriginStorageDelete>(operation, std::move(origin), id));
  NextOriginStorageDelete();
}
