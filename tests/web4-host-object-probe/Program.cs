// CI-only frame host-object/origin experiment; not the Gesher bridge.
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;

static class Program
{
    [STAThread] static int Main()
    {
        ApplicationConfiguration.Initialize(); using var form = new ProbeForm(); Application.Run(form); return form.Result;
    }
}

[ComVisible(true), ClassInterface(ClassInterfaceType.AutoDual)]
public sealed class ProbeObject
{
    public record Call(int ObjectId, string Attempt, bool Retired);
    [ComVisible(false)] public static readonly ConcurrentQueue<Call> Calls = new();
    readonly int id;
    int retired;
    public ProbeObject(int id) { this.id = id; }
    [ComVisible(false)] public int Id => id;
    [ComVisible(false)] public void Retire() => Interlocked.Exchange(ref retired, 1);
    public string Ping(string attempt)
    {
        bool isRetired = Volatile.Read(ref retired) != 0;
        Calls.Enqueue(new Call(id, attempt, isRetired));
        return isRetired ? "retired" : "ok:" + id;
    }
}

sealed class ProbeForm : Form
{
    sealed class FrameState
    {
        public required string Name;
        public required CoreWebView2Frame Frame;
        public required ProbeObject Current;
        public bool Loaded;
        public int NavigationStarts;
        public string LastNavigation = "";
    }
    record Context(string Session, int Id, string Unique, string Origin, string Frame, bool Default);
    readonly ConcurrentDictionary<(string, int), Context> contexts = new();
    readonly ConcurrentDictionary<string, Context> observations = new();
    readonly ConcurrentDictionary<string, bool> destroyed = new();
    readonly ConcurrentDictionary<string, string> parents = new();
    readonly ConcurrentDictionary<string, string> targetTypes = new();
    readonly ConcurrentDictionary<string, Task> sessionSetup = new();
    readonly List<CoreWebView2DevToolsProtocolEventReceiver> receivers = new();
    bool eventFailure;
    readonly WebView2 web = new() { Dock = DockStyle.Fill };
    readonly ConcurrentDictionary<string, FrameState> frames = new();
    readonly ConcurrentDictionary<string, string> pages = new();
    readonly TcpListener server = new(IPAddress.Any, 0);
    readonly TaskCompletionSource held = new(TaskCreationOptions.RunContinuationsAsynchronously);
    readonly TaskCompletionSource release = new(TaskCreationOptions.RunContinuationsAsynchronously);
    int port, nextObject;
    bool failure;
    public int Result { get; private set; } = 1;
    ProbeObject NewObject() => new(Interlocked.Increment(ref nextObject));

    public ProbeForm() { Controls.Add(web); Width = 400; Height = 300; Shown += async (_, _) => await Run(); }

    async Task Run()
    {
        using var limit = new CancellationTokenSource(TimeSpan.FromSeconds(75));
        try
        {
            server.Start(); port = ((IPEndPoint)server.LocalEndpoint).Port; _ = Serve(limit.Token);
            var env = await CoreWebView2Environment.CreateAsync(null, Path.Combine(Path.GetTempPath(), "web4-host-" + Guid.NewGuid()));
            await web.EnsureCoreWebView2Async(env);
            Console.WriteLine("RUNTIME " + env.BrowserVersionString);
            var core = web.CoreWebView2;
            foreach (string name in new[] { "Runtime.executionContextCreated", "Runtime.executionContextDestroyed", "Runtime.executionContextsCleared", "Runtime.bindingCalled", "Page.frameAttached", "Target.attachedToTarget", "Target.detachedFromTarget" })
            {
                var receiver = core.GetDevToolsProtocolEventReceiver(name);
                receiver.DevToolsProtocolEventReceived += (_, e) => Event(name, e.SessionId ?? "", e.ParameterObjectAsJson);
                receivers.Add(receiver);
            }
            await core.CallDevToolsProtocolMethodAsync("Runtime.enable", "{}");
            await core.CallDevToolsProtocolMethodAsync("Page.enable", "{}");
            await core.CallDevToolsProtocolMethodAsync("Runtime.addBinding", "{\"name\":\"realmProbe\"}");
            await core.CallDevToolsProtocolMethodAsync("Target.setAutoAttach", "{\"autoAttach\":true,\"waitForDebuggerOnStart\":true,\"flatten\":true}");
            var topObject = NewObject();
            core.AddHostObjectToScript("rootProbe", topObject);
            core.FrameCreated += (_, e) => RegisterFrame(e.Frame);
            core.Navigate($"http://127.0.0.1:{port}/main");
            string[] selected = { "a", "b", "nested", "bad", "opaque" };
            await Wait(() => observations.ContainsKey("opaque") && pages.ContainsKey("opaque"), TimeSpan.FromSeconds(15), limit.Token);
            var opaqueContext = observations["opaque"];
            if (!opaqueContext.Default || opaqueContext.Unique.Length == 0 || opaqueContext.Frame.Length == 0 || opaqueContext.Origin != "://" || eventFailure)
                throw new Exception("selected opaque CDP context identity/origin missing");
            Console.WriteLine($"OPAQUE_CDP session={opaqueContext.Session} context={opaqueContext.Id} unique={opaqueContext.Unique} frame={opaqueContext.Frame} origin={opaqueContext.Origin} nativeFrameId={frames["opaque"].Frame.FrameId} correlation=controlled-label-only");
            await Wait(() => selected.All(pages.ContainsKey) && selected.All(frames.ContainsKey) &&
                ProbeObject.Calls.Any(c => c.Attempt == "root:first"), TimeSpan.FromSeconds(15), limit.Token);
            if (selected.Select(name => frames[name].Frame.FrameId).Any(id => id == 0) ||
                selected.Select(name => frames[name].Frame.FrameId).Distinct().Count() != selected.Length)
                throw new Exception("frame native IDs are missing or ambiguous");
            var rootFirst = ProbeObject.Calls.Where(c => c.Attempt == "root:first").ToArray();
            if (rootFirst.Length != 1 || rootFirst[0].ObjectId != topObject.Id || rootFirst[0].Retired)
                throw new Exception("top-level first-script control mismatch");
            Console.WriteLine("TOP_FIRST object=" + topObject.Id);
            foreach (string name in new[] { "a", "b", "nested" })
            {
                var state = frames[name];
                var matches = ProbeObject.Calls.Where(c => c.Attempt == "first:" + name).ToArray();
                if (matches.Length != 1 || matches[0].ObjectId != state.Current.Id || matches[0].Retired ||
                    pages[name] != "OK|ok:" + state.Current.Id) throw new Exception("allowed early host call mismatch: " + name);
                Console.WriteLine($"ALLOWED_EARLY frame={name} frameId={state.Frame.FrameId} object={state.Current.Id}");
            }
            foreach (string name in new[] { "bad", "opaque" })
            {
                var state = frames[name];
                if (!pages[name].StartsWith("DENIED|", StringComparison.Ordinal) ||
                    ProbeObject.Calls.Any(c => c.ObjectId == state.Current.Id)) throw new Exception("denied host access not proven: " + name);
                Console.WriteLine($"DENIED_ATTEMPT frame={name} frameId={state.Frame.FrameId} object={state.Current.Id} page={pages[name]} nativeCalls=0");
            }
            FrameState a = frames["a"];
            int oldId = a.Current.Id;
            int beforeNavigation = a.NavigationStarts;
            await core.ExecuteScriptAsync("window.savedProxy=document.querySelector('iframe[name=a]').contentWindow.chrome.webview.hostObjects.sync.probe;document.querySelector('iframe[name=a]').src='/held?name=a2'").WaitAsync(TimeSpan.FromSeconds(3), limit.Token);
            await held.Task.WaitAsync(TimeSpan.FromSeconds(3), limit.Token);
            if (a.NavigationStarts <= beforeNavigation || !a.LastNavigation.Contains("/held?name=a2", StringComparison.Ordinal))
                throw new Exception("held frame navigation-start event was not observed");
            string during = await core.ExecuteScriptAsync("window.savedProxy.Ping('during-held')").WaitAsync(TimeSpan.FromSeconds(3), limit.Token);
            var heldCalls = ProbeObject.Calls.Where(c => c.Attempt == "during-held").ToArray();
            if (heldCalls.Length != 1 || heldCalls[0].ObjectId != oldId || heldCalls[0].Retired || JsonDocument.Parse(during).RootElement.GetString() != "ok:" + oldId)
                throw new Exception("old frame proxy did not remain valid while navigation was held");
            Console.WriteLine($"HELD_OLD_ACCEPTED frameId={a.Frame.FrameId} object={oldId}");
            release.TrySetResult();
            await Wait(() => pages.ContainsKey("a2") && ProbeObject.Calls.Any(c => c.Attempt == "first:a2"), TimeSpan.FromSeconds(3), limit.Token);
            var replacement = a.Current;
            var newCalls = ProbeObject.Calls.Where(c => c.Attempt == "first:a2").ToArray();
            if (replacement.Id == oldId || newCalls.Length != 1 || newCalls[0].ObjectId != replacement.Id || newCalls[0].Retired || pages["a2"] != "OK|ok:" + replacement.Id)
                throw new Exception("replacement document object identity mismatch");
            string post = await core.ExecuteScriptAsync("(()=>{try{return window.savedProxy.Ping('post-commit')}catch(e){return 'PROXY_EXCEPTION:'+String(e)}})()").WaitAsync(TimeSpan.FromSeconds(3), limit.Token);
            string value = JsonDocument.Parse(post).RootElement.GetString() ?? "";
            var oldCalls = ProbeObject.Calls.Where(c => c.Attempt == "post-commit").ToArray();
            bool retiredRejected = value == "retired" && oldCalls.Length == 1 && oldCalls[0].ObjectId == oldId && oldCalls[0].Retired;
            bool proxyException = value.StartsWith("PROXY_EXCEPTION:", StringComparison.Ordinal) && oldCalls.Length == 0;
            if (!retiredRejected && !proxyException) throw new Exception("old proxy reached an accepted document: " + value);
            Console.WriteLine($"OLD_PROXY_RESULT old={oldId} replacement={replacement.Id} nativeRetired={retiredRejected} proxyException={proxyException} value={value}");
            core.Navigate($"http://localhost:{port}/other");
            await Wait(() => ProbeObject.Calls.Any(c => c.Attempt == "root:cross"), TimeSpan.FromSeconds(3), limit.Token);
            var rootCalls = ProbeObject.Calls.Where(c => c.Attempt == "root:cross").ToArray();
            if (rootCalls.Length != 1 || rootCalls[0].ObjectId != topObject.Id || rootCalls[0].Retired)
                throw new Exception("top-level unscoped host-object control missing");
            Console.WriteLine($"TOP_CROSS_UNSCOPED object={topObject.Id} origin=http://localhost:{port}");
            if (failure) throw new Exception("frame registration or page observation failed");
            Console.WriteLine("HOST_OBJECT_PROBE_PASS selected exact-origin frame calls, denied attempts, held proxy retirement and unscoped top-level control; broader rules and production unproven");
            Result = 0;
        }
        catch (Exception e) { Console.WriteLine("HOST_OBJECT_PROBE_FAILURE " + e); }
        finally {
            foreach (var call in ProbeObject.Calls) Console.WriteLine($"NATIVE_CALL object={call.ObjectId} attempt={call.Attempt} retired={call.Retired}");
            release.TrySetResult(); limit.Cancel(); server.Stop(); Close();
        }
    }

    void Event(string name, string session, string json)
    {
        Console.WriteLine("CDP " + name + " session=" + session + " " + json);
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            if (name == "Page.frameAttached")
                parents[root.GetProperty("frameId").GetString() ?? ""] = root.GetProperty("parentFrameId").GetString() ?? "";
            else if (name == "Target.attachedToTarget")
            {
                string child = root.GetProperty("sessionId").GetString() ?? "";
                targetTypes[child] = root.GetProperty("targetInfo").GetProperty("type").GetString() ?? "";
                sessionSetup[child] = SetupSession(child);
            }
            else if (name == "Runtime.executionContextCreated")
            {
                var c = root.GetProperty("context");
                var aux = c.TryGetProperty("auxData", out var a) ? a : default;
                int id = c.GetProperty("id").GetInt32();
                string unique = c.TryGetProperty("uniqueId", out var u) ? u.GetString() ?? "" : "";
                if (!c.TryGetProperty("origin", out var o) || o.ValueKind != JsonValueKind.String)
                    throw new Exception("context origin property is missing or not a string");
                string origin = o.GetString()!;
                string frame = aux.ValueKind == JsonValueKind.Object && aux.TryGetProperty("frameId", out var f) ? f.GetString() ?? "" : "";
                bool main = aux.ValueKind == JsonValueKind.Object && aux.TryGetProperty("isDefault", out var d) && d.ValueKind == JsonValueKind.True;
                contexts[(session, id)] = new Context(session, id, unique, origin, frame, main);
            }
            else if (name == "Runtime.bindingCalled")
            {
                if (root.GetProperty("name").GetString() != "realmProbe") return;
                int id = root.GetProperty("executionContextId").GetInt32();
                string label = root.GetProperty("payload").GetString() ?? "";
                if (contexts.TryGetValue((session, id), out var c))
                {
                    if (!observations.TryAdd(label, c)) { eventFailure = true; Console.WriteLine("DUPLICATE_BINDING " + label); }
                }
                else { eventFailure = true; Console.WriteLine("UNMATCHED_BINDING " + label); }
            }
            else if (name == "Runtime.executionContextDestroyed")
            {
                if (root.TryGetProperty("executionContextUniqueId", out var unique)) destroyed[unique.GetString() ?? ""] = true;
                if (root.TryGetProperty("executionContextId", out var id) && contexts.TryRemove((session, id.GetInt32()), out var old)) destroyed[old.Unique] = true;
            }
            else if (name == "Runtime.executionContextsCleared")
                foreach (var key in contexts.Keys.Where(k => k.Item1 == session)) contexts.TryRemove(key, out _);
        }
        catch (Exception e) { eventFailure = true; Console.WriteLine("CDP_EVENT_FAILURE " + e); }
    }

    async Task SetupSession(string session)
    {
        try
        {
            var core = web.CoreWebView2;
            await core.CallDevToolsProtocolMethodForSessionAsync(session, "Runtime.enable", "{}");
            await core.CallDevToolsProtocolMethodForSessionAsync(session, "Page.enable", "{}");
            await core.CallDevToolsProtocolMethodForSessionAsync(session, "Runtime.addBinding", "{\"name\":\"realmProbe\"}");
            await core.CallDevToolsProtocolMethodForSessionAsync(session, "Target.setAutoAttach", "{\"autoAttach\":true,\"waitForDebuggerOnStart\":true,\"flatten\":true}");
            await core.CallDevToolsProtocolMethodForSessionAsync(session, "Runtime.runIfWaitingForDebugger", "{}");
        }
        catch (Exception e) { eventFailure = true; Console.WriteLine("SESSION_SETUP_FAILURE " + session + " " + e); }
    }

    void RegisterFrame(CoreWebView2Frame frame)
    {
        try
        {
            string name = frame.Name;
            var state = new FrameState { Name = name, Frame = frame, Current = NewObject() };
            if (!frames.TryAdd(name, state)) throw new Exception("duplicate or missing frame name: " + name);
            Install(state);
            frame.WebMessageReceived += (_, e) => {
                try {
                    string text = e.TryGetWebMessageAsString();
                    string[] parts = text.Split('|', 3);
                    string expected = state.Name == "a" && state.LastNavigation.Contains("/held?name=a2", StringComparison.Ordinal) ? "a2" : state.Name;
                    if (parts.Length != 3 || parts[1] != expected || !pages.TryAdd(parts[1], parts[0] + "|" + parts[2])) failure = true;
                    Console.WriteLine($"FRAME_OBSERVATION frame={state.Name} frameId={frame.FrameId} payload={text}");
                } catch (Exception ex) { failure = true; Console.WriteLine("FRAME_OBSERVATION_FAILURE " + ex); }
            };
            try { frame.FrameCreated += (_, e) => RegisterFrame(e.Frame); }
            catch (NotImplementedException e) { failure = true; Console.WriteLine("NESTED_FRAME_CAPABILITY_FAILURE " + e); }
            frame.NavigationStarting += (_, e) => {
                state.NavigationStarts++;
                state.LastNavigation = e.Uri;
                Console.WriteLine($"FRAME_NAV_START frame={state.Name} frameId={frame.FrameId} uri={e.Uri}");
            };
            frame.ContentLoading += (_, _) => {
                try {
                    if (state.Loaded) { state.Current.Retire(); state.Current = NewObject(); Install(state); }
                    state.Loaded = true;
                    Console.WriteLine($"FRAME_CONTENT frame={state.Name} frameId={frame.FrameId} object={state.Current.Id}");
                } catch (Exception e) { failure = true; Console.WriteLine("FRAME_CONTENT_FAILURE " + e); }
            };
            frame.Destroyed += (_, _) => { state.Current.Retire(); Console.WriteLine("FRAME_DESTROYED " + name); };
            Console.WriteLine($"FRAME_REGISTERED name={name} frameId={frame.FrameId} object={state.Current.Id}");
        }
        catch (Exception e) { failure = true; Console.WriteLine("FRAME_REGISTRATION_FAILURE " + e); }
    }

    void Install(FrameState state) => state.Frame.AddHostObjectToScript("probe", state.Current, new[] { $"http://127.0.0.1:{port}" });

    static async Task Wait(Func<bool> condition, TimeSpan budget, CancellationToken outer)
    {
        long start = Stopwatch.GetTimestamp();
        while (!condition())
        {
            outer.ThrowIfCancellationRequested();
            if (Stopwatch.GetElapsedTime(start) >= budget) throw new TimeoutException("phase outcome deadline exceeded");
            await Task.Delay(20, outer);
        }
        if (Stopwatch.GetElapsedTime(start) >= budget) throw new TimeoutException("phase outcome observed after deadline");
    }

    async Task Serve(CancellationToken token)
    {
        while (!token.IsCancellationRequested)
        {
            TcpClient client;
            try { client = await server.AcceptTcpClientAsync(token); } catch (OperationCanceledException) { return; }
            _ = Respond(client);
        }
    }
    async Task Respond(TcpClient client)
    {
        using (client)
        {
            var stream = client.GetStream(); using var reader = new StreamReader(stream, Encoding.ASCII, false, 1024, true);
            string line = await reader.ReadLineAsync() ?? ""; string path = line.Split(' ').Skip(1).FirstOrDefault() ?? "/";
            while (!string.IsNullOrEmpty(await reader.ReadLineAsync())) { }
            if (path.StartsWith("/held")) { held.TrySetResult(); await release.Task.WaitAsync(TimeSpan.FromSeconds(15)); }
            string child = "<script>let n=new URLSearchParams(location.search).get('name');realmProbe(n);if(!window.chrome||!chrome.webview||!chrome.webview.hostObjects||!chrome.webview.hostObjects.sync){chrome.webview.postMessage('INFRA|'+n+'|missing host namespace')}else{try{let p=chrome.webview.hostObjects.sync.probe;if(!p){chrome.webview.postMessage('INFRA|'+n+'|missing proxy')}else{try{let v=p.Ping('first:'+n);chrome.webview.postMessage('OK|'+n+'|'+v)}catch(e){chrome.webview.postMessage('DENIED|'+n+'|CALL:'+String(e))}}}catch(e){chrome.webview.postMessage('DENIED|'+n+'|GET:'+String(e))}}</script>";
            string html = path.StartsWith("/main")
                ? $"<script>chrome.webview.hostObjects.sync.rootProbe.Ping('root:first')</script><iframe name='a' src='/child?name=a'></iframe><iframe name='b' src='/child?name=b'></iframe><iframe name='bad' src='http://localhost:{port}/child?name=bad'></iframe><iframe name='opaque' sandbox='allow-scripts' src='/child?name=opaque'></iframe>"
                : path.StartsWith("/other") ? "<script>chrome.webview.hostObjects.sync.rootProbe.Ping('root:cross')</script>"
                : child + (path.EndsWith("name=a") ? "<iframe name='nested' src='/child?name=nested'></iframe>" : "");
            byte[] body = Encoding.UTF8.GetBytes("<!doctype html>" + html);
            byte[] head = Encoding.ASCII.GetBytes($"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {body.Length}\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n");
            await stream.WriteAsync(head); await stream.WriteAsync(body);
        }
    }
}
