"""Exact insertion-only observation overlay; removing the additions restores source."""

HOST = 'compat/webview/webview_host.cc'
JNI = 'compat/webview/webview_jni.cc'

# Each anchor is required exactly once. Calls only observe already available
# scalars. They do not add native events, navigation, waits or success markers.
INSERTIONS = {
    HOST: [
        ('#include "owned_stream.h"\n', '#include "phase_trace.h"\n'),
        ('DWORD WINAPI ThreadMain(void* ready) {\n', '  QaNotesPhase("sta_entry");\n'),
        ('void WithEnvironment(std::function<void(ICoreWebView2Environment*, HRESULT)> fn) {\n',
         '  QaNotesPhase("environment_enter", nullptr, static_cast<int>(g_env_state));\n'),
        ('  auto finish = [](HRESULT hr, ICoreWebView2Environment* env) {\n',
         '    QaNotesPhase("environment_callback", env, hr);\n'),
        ('  HRESULT hr = CreateEnvironment(nullptr, user_data.empty() ? nullptr : user_data.c_str(),\n',
         None),
        ('                                 options.get(), handler.get());\n',
         '  QaNotesPhase("environment_return", nullptr, hr);\n'),
        ('              const std::vector<uint8_t>* bytes = nullptr) {\n',
         '    QaNotesPhase("report_event", this, type, id, arg, destroyed || closing);\n'),
        ('void Fail(Impl* impl, HRESULT hr, const std::wstring& what) {\n',
         '  QaNotesPhase("failed", impl, hr);\n'),
        ('    if (!impl->ready) {\n',
         '      QaNotesPhase("work_queued", impl.get(), impl->pending.size());\n'),
        ('void ApplyBounds(Impl* impl) {\n',
         '  QaNotesPhase("bounds_apply", impl, impl->has_bounds, impl->visible, impl->bounds.right - impl->bounds.left, impl->bounds.bottom - impl->bounds.top);\n'),
        ('                        args->get_IsRedirected(&redirect);\n',
         '                        QaNotesPhase("navigation_starting", impl.get(), user, redirect, impl->api_navigation);\n'),
        ('                        args->get_WebErrorStatus(&status);\n',
         '                        QaNotesPhase("navigation_completed", impl.get(), nav, ok, status);\n'),
        ('                        bool main = document && url == impl->main_document;\n',
         '                        QaNotesPhase("resource_dispatch", impl.get(), id, context, main, file);\n'),
        ('                  ICoreWebView2Controller* controller) {\n',
         '  QaNotesPhase("controller_callback", impl.get(), hr, controller != nullptr, impl->destroyed);\n'),
        ('  if (!AddHandlers(impl)) {\n', None),
        ('  impl->ready = true;\n',
         '  QaNotesPhase("ready", impl.get(), impl->pending.size());\n'),
        ('  for (auto& fn : pending) {\n',
         '    QaNotesPhase("pending_dispatch", impl.get(), impl->destroyed);\n'),
        ('void Start(const std::shared_ptr<Impl>& impl) {\n',
         '  QaNotesPhase("host_start", impl.get(), reinterpret_cast<intptr_t>(impl->options.parent));\n'),
        ('  impl->published_container.store(impl->container);\n',
         '  QaNotesPhase("container_created", impl.get(), reinterpret_cast<intptr_t>(impl->container), reinterpret_cast<intptr_t>(parent));\n'),
        ('    g_controller_queue.push_back([impl] {\n',
         '      QaNotesPhase("controller_request", impl.get());\n'),
        ('      if (FAILED(created)) {\n',
         None),
        ('                    const std::wstring& method, std::vector<uint8_t> body) {\n',
         '  QaNotesPhase("navigate_enqueue", impl_.get());\n'),
        ('    impl->api_navigation = true;\n    HRESULT hr = E_FAIL;\n',
         '    QaNotesPhase("navigate_begin", impl);\n'),
        ('      hr = impl->webview->Navigate(url.c_str());\n    }\n',
         '    QaNotesPhase("navigate_return", impl, hr);\n'),
        ('void Host::NavigateToString(const std::wstring& html) {\n',
         '  QaNotesPhase("navigate_string_enqueue", impl_.get());\n'),
        ('    HRESULT hr = impl->webview->NavigateToString(html.c_str());\n',
         '    QaNotesPhase("navigate_string_return", impl, hr);\n'),
        ('    bool shown = visible && parent != nullptr && clip_width > 0 && clip_height > 0;\n',
         '    QaNotesPhase("bounds_received", impl, shown, width, height, reinterpret_cast<intptr_t>(parent));\n'),
        ('    uint64_t generation = ++impl->document_script_generation;\n',
         '    QaNotesPhase("document_script_request", impl, generation, script.empty());\n'),
        ('                        [weak, generation](HRESULT error, LPCWSTR id) -> HRESULT {\n',
         '                          QaNotesPhase("document_script_callback", nullptr, error, generation, id != nullptr);\n'),
        ('                            const std::wstring& headers, std::vector<uint8_t> body) {\n',
         '  QaNotesPhase("resource_response_enqueue", impl_.get(), id, status, body.size());\n'),
        ('  PostToWebViewThread([shared, id, status, reason, headers, body = std::move(bytes)] {\n',
         '    QaNotesPhase("resource_response_begin", shared.get(), id, status);\n'),
        ('    r.deferral->Complete();\n  });\n}\n\nvoid Host::CompleteDialog',
         None),
    ],
    JNI: [
        ('#include "webview_host.h"\n', '#include "phase_trace.h"\n'),
        ('               const std::wstring& c, const std::vector<uint8_t>* bytes) override {\n',
         '    QaNotesPhase("jni_event_enter", this, type, id, arg);\n'),
        ('    env->CallVoidMethod(peer_, g_on_event, type, static_cast<jlong>(id), arg, ja, jb, jc, data);\n',
         '    QaNotesPhase("jni_event_return", this, type, id, arg, env->ExceptionCheck());\n'),
        ('                         const std::wstring& headers, int32_t context, bool main_frame) override {\n',
         '    QaNotesPhase("jni_resource_enter", this, id, context, main_frame);\n'),
        ('                        main_frame ? JNI_TRUE : JNI_FALSE);\n',
         '    QaNotesPhase("jni_resource_return", this, id, env->ExceptionCheck());\n'),
        ('  Host* host = Host::Create(listener, options);\n',
         '  QaNotesPhase("jni_create_return", host, reinterpret_cast<intptr_t>(listener), parent);\n'),
        ('  std::wstring m = Str(env, method);\n',
         '  QaNotesPhase("jni_navigate", Get(h));\n'),
        ('extern "C" jint JNI_OnLoad_gesher_webview(JavaVM* vm, void*) {\n',
         '  QaNotesPhase("jni_onload", vm);\n'),
    ],
}

# Insert before these anchors, avoiding changes to the original call statements.
BEFORE = {
    HOST: [
        ('  HRESULT hr = CreateEnvironment(nullptr, user_data.empty() ? nullptr : user_data.c_str(),\n',
         '  QaNotesPhase("environment_request");\n'),
        ('  if (!AddHandlers(impl)) {\n', '  QaNotesPhase("handlers_begin", impl.get());\n'),
        ('      if (FAILED(created)) {\n', '      QaNotesPhase("controller_return", impl.get(), created);\n'),
        ('    r.deferral->Complete();\n  });\n}\n\nvoid Host::CompleteDialog',
         '    QaNotesPhase("resource_response_complete", impl, id, status);\n'),
    ]
}

def apply(path, original):
    value = original
    additions = []
    for anchor, addition in INSERTIONS.get(path, []):
        if addition is None:
            continue
        if value.count(anchor) != 1:
            raise ValueError(('Observation anchor is not unique', path, anchor))
        value = value.replace(anchor, anchor + addition, 1)
        additions.append(addition)
    for anchor, addition in BEFORE.get(path, []):
        if value.count(anchor) != 1:
            raise ValueError(('Observation anchor is not unique', path, anchor))
        value = value.replace(anchor, addition + anchor, 1)
        additions.append(addition)
    restored = value
    for addition in reversed(additions):
        if restored.count(addition) != 1:
            raise ValueError(('Observation removal is not unique', path, addition))
        restored = restored.replace(addition, '', 1)
    if restored != original:
        raise ValueError('Observation overlay changed original source: ' + path)
    return value, additions
