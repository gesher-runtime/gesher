// CI-only WebView2/CDP early-injection experiment. Not the Gesher bridge.
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;

static class Program
{
    [STAThread]
    static int Main()
    {
        ApplicationConfiguration.Initialize();
        using var form = new ProbeForm();
        Application.Run(form);
        return form.Result;
    }
}

sealed class ProbeForm : Form
{
    sealed class ContextGoneException : Exception
    {
        public ContextGoneException(string message) : base(message) { }
    }
    readonly WebView2 web = new() { Dock = DockStyle.Fill };
    readonly ConcurrentDictionary<(string, int), Context> contexts = new();
    readonly ConcurrentDictionary<string, Context> observations = new();
    readonly ConcurrentDictionary<string, bool> destroyed = new();
    readonly ConcurrentDictionary<string, string> parents = new();
    readonly ConcurrentDictionary<string, string> targetTypes = new();
    readonly ConcurrentDictionary<(string, string), int> scriptContexts = new();
    readonly ConcurrentDictionary<string, bool> initialized = new();
    readonly ConcurrentDictionary<string, string> remoteObjects = new();
    readonly ConcurrentDictionary<string, string> exposures = new();
    readonly ConcurrentDictionary<string, Context> rejectedForgeries = new();
    sealed class ResumeWait { public bool Issued; public readonly TaskCompletionSource Signal = new(TaskCreationOptions.RunContinuationsAsynchronously); }
    readonly ConcurrentDictionary<string, ResumeWait> resumeSignals = new();
    readonly ConcurrentBag<Task> pauseTasks = new();
    readonly ConcurrentDictionary<string, Task> sessionSetup = new();
    readonly List<CoreWebView2DevToolsProtocolEventReceiver> receivers = new();
    readonly TcpListener server = new(IPAddress.Any, 0);
    readonly TaskCompletionSource holdEntered = new(TaskCreationOptions.RunContinuationsAsynchronously);
    readonly TaskCompletionSource holdRelease = new(TaskCreationOptions.RunContinuationsAsynchronously);
    int port;
    bool eventFailure;
    public int Result { get; private set; } = 1;
    record Context(string Session, int Id, string Unique, string Origin, string Frame, bool Default);

    public ProbeForm()
    {
        Controls.Add(web);
        Width = 400; Height = 300;
        Shown += async (_, _) => await Run();
    }

    async Task Run()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(75));
        try
        {
            server.Start();
            port = ((IPEndPoint)server.LocalEndpoint).Port;
            _ = Serve(timeout.Token);
            string profile = Path.Combine(Path.GetTempPath(), "web4-realm-" + Guid.NewGuid());
            var env = await CoreWebView2Environment.CreateAsync(null, profile);
            await web.EnsureCoreWebView2Async(env);
            var core = web.CoreWebView2;
            Console.WriteLine("RUNTIME " + env.BrowserVersionString);
            foreach (string name in new[] { "Runtime.executionContextCreated", "Runtime.executionContextDestroyed", "Runtime.executionContextsCleared", "Runtime.bindingCalled", "Debugger.scriptParsed", "Debugger.paused", "Debugger.resumed", "Page.frameAttached", "Target.attachedToTarget", "Target.detachedFromTarget" })
            {
                var receiver = core.GetDevToolsProtocolEventReceiver(name);
                receiver.DevToolsProtocolEventReceived += (_, e) => Event(name, e.SessionId ?? "", e.ParameterObjectAsJson);
                receivers.Add(receiver);
            }
            await core.CallDevToolsProtocolMethodAsync("Runtime.enable", "{}");
            await core.CallDevToolsProtocolMethodAsync("Page.enable", "{}");
            await core.CallDevToolsProtocolMethodAsync("Runtime.addBinding", "{\"name\":\"realmProbe\"}");
            await core.CallDevToolsProtocolMethodAsync("Debugger.enable", "{}");
            await core.CallDevToolsProtocolMethodAsync("Debugger.setInstrumentationBreakpoint", "{\"instrumentation\":\"beforeScriptExecution\"}");
            await core.CallDevToolsProtocolMethodAsync("Target.setAutoAttach", "{\"autoAttach\":true,\"waitForDebuggerOnStart\":true,\"flatten\":true}");
            web.Source = new Uri($"http://127.0.0.1:{port}/main");
            string[] expected = { "main", "a", "b", "nested", "cross", "nestedCross", "sandbox", "blank", "srcdoc" };
            string[] allowed = { "main", "a", "b", "nested", "blank", "srcdoc" };
            string[] denied = { "cross", "nestedCross", "sandbox" };
            while (!expected.All(label => observations.ContainsKey(label) && observations.ContainsKey("EXPOSURE:" + label)) ||
                   !allowed.All(label => observations.ContainsKey("object-call:" + label)) ||
                   !denied.All(rejectedForgeries.ContainsKey) || !observations.ContainsKey("replaced-b"))
            {
                timeout.Token.ThrowIfCancellationRequested();
                await Task.Delay(50, timeout.Token);
            }
            foreach (string label in expected)
            {
                Context c = observations[label];
                if (!c.Default || c.Unique.Length == 0 || c.Frame.Length == 0) throw new Exception("missing default context identity for " + label);
                Console.WriteLine($"OBS {label} session={c.Session} id={c.Id} unique={c.Unique} origin={c.Origin} frame={c.Frame}");
                if (observations["EXPOSURE:" + label].Unique != c.Unique)
                    throw new Exception("exposure observation came from a different context: " + label);
            }
            foreach (string label in allowed)
                if (!exposures.TryGetValue(label, out var value) || value != "object" ||
                    observations["object-call:" + label].Unique != observations[label].Unique)
                    throw new Exception("allowed object absent or wrong realm before page script: " + label);
            foreach (string label in denied)
                if (!exposures.TryGetValue(label, out var value) || value != "undefined" ||
                    rejectedForgeries[label].Unique != observations[label].Unique ||
                    rejectedForgeries[label].Session != observations[label].Session)
                    throw new Exception("unallowed exposure or forged sender mismatch: " + label);
            if (observations["replaced-b"].Unique != observations["b"].Unique) throw new Exception("replacement control came from wrong realm");
            foreach (string label in allowed)
            {
                Context c = observations[label];
                if (!remoteObjects.TryGetValue(c.Unique, out var objectId)) throw new Exception("missing original injected object: " + label);
                await CallOriginal(c, objectId, "probe");
            }
            while (!allowed.All(label => observations.ContainsKey("early-reply:" + label + ":probe")))
            { timeout.Token.ThrowIfCancellationRequested(); await Task.Delay(50, timeout.Token); }
            foreach (string label in allowed)
                if (observations["early-reply:" + label + ":probe"].Unique != observations[label].Unique)
                    throw new Exception("original-object reply reached wrong context: " + label);
            if (observations.ContainsKey("wrong-object")) throw new Exception("reply used the page's replacement object");
            if (observations["a"].Unique == observations["b"].Unique || observations["a"].Frame == observations["b"].Frame)
                throw new Exception("sibling contexts or frames are not distinct");
            if (!targetTypes.TryGetValue(observations["cross"].Session, out var crossType) || crossType != "iframe")
                throw new Exception("cross-origin iframe target session was not observed");
            if (observations["nestedCross"].Session.Length == 0 || observations["nestedCross"].Frame == observations["cross"].Frame)
                throw new Exception("distinct nested cross-origin target was not observed");
            if (!targetTypes.TryGetValue(observations["nestedCross"].Session, out var nestedType) || nestedType != "iframe")
                throw new Exception("nested cross-origin iframe target session was not observed");
            if (!parents.TryGetValue(observations["nested"].Frame, out var nestedParent) || nestedParent != observations["a"].Frame ||
                !parents.TryGetValue(observations["nestedCross"].Frame, out var crossParent) || crossParent != observations["a"].Frame)
                throw new Exception("engine frame attachment did not establish nested parentage");
            if (observations["main"].Origin != $"http://127.0.0.1:{port}" ||
                observations["cross"].Origin != $"http://localhost:{port}")
                throw new Exception("network context origins differ from controlled origins");
            if (!new[] { "", "null", "://" }.Contains(observations["sandbox"].Origin))
                throw new Exception("sandbox origin was not observed as opaque");
            if (observations["blank"].Origin != observations["main"].Origin || observations["srcdoc"].Origin != observations["main"].Origin)
                throw new Exception("inherited-origin observations differ from main");
            foreach (string label in expected)
                await Evaluate(observations[label], "realmProbe(" + JsonSerializer.Serialize("reply-" + label) + ")");
            while (!expected.All(label => observations.ContainsKey("reply-" + label)))
            {
                timeout.Token.ThrowIfCancellationRequested(); await Task.Delay(50, timeout.Token);
            }
            foreach (string label in expected)
                if (observations["reply-" + label].Unique != observations[label].Unique)
                    throw new Exception("reply reached a different context: " + label);
            Context old = observations["a"];
            await Evaluate(observations["main"], "document.getElementById('a').src='/held?name=a2'");
            await holdEntered.Task.WaitAsync(timeout.Token);
            await Evaluate(old, "realmProbe('during-held')");
            while (!observations.ContainsKey("during-held"))
            { timeout.Token.ThrowIfCancellationRequested(); await Task.Delay(50, timeout.Token); }
            if (observations["during-held"].Unique != old.Unique) throw new Exception("held message attributed to another context");
            Console.WriteLine($"OBS during-held session={old.Session} id={old.Id} unique={old.Unique} origin={old.Origin} frame={old.Frame}");
            holdRelease.TrySetResult();
            while (!observations.ContainsKey("a2") || !destroyed.ContainsKey(old.Unique))
            {
                timeout.Token.ThrowIfCancellationRequested(); await Task.Delay(50, timeout.Token);
            }
            Context replacement = observations["a2"];
            if (!replacement.Default || replacement.Unique.Length == 0 || replacement.Frame.Length == 0 ||
                replacement.Unique == old.Unique || replacement.Frame != old.Frame || replacement.Session != old.Session)
                throw new Exception("replacement context identity does not establish same-frame new document");
            Console.WriteLine($"OBS a2 session={replacement.Session} id={replacement.Id} unique={replacement.Unique} origin={replacement.Origin} frame={replacement.Frame}");
            while (!remoteObjects.ContainsKey(replacement.Unique) || !observations.ContainsKey("object-call:a2"))
            { timeout.Token.ThrowIfCancellationRequested(); await Task.Delay(50, timeout.Token); }
            if (observations["object-call:a2"].Unique != replacement.Unique || !remoteObjects.TryGetValue(old.Unique, out var oldObject))
                throw new Exception("replacement or old injected-object identity missing");
            await CallOriginal(replacement, remoteObjects[replacement.Unique], "same");
            while (!observations.ContainsKey("early-reply:a2:same"))
            { timeout.Token.ThrowIfCancellationRequested(); await Task.Delay(50, timeout.Token); }
            if (observations["early-reply:a2:same"].Unique != replacement.Unique ||
                !observations.TryRemove("early-reply:a2:same", out _)) throw new Exception("live original-object control ambiguous");
            bool oldObjectRejected = false;
            try { await CallOriginal(old, oldObject, "same"); }
            catch (ArgumentException e) when (e.GetType() == typeof(ArgumentException) && e.HResult == unchecked((int)0x80070057))
            { Console.WriteLine($"OLD_OBJECT_REJECTED type={e.GetType().FullName} hresult=0x{e.HResult:X8}"); oldObjectRejected = true; }
            if (!oldObjectRejected) throw new Exception("old object operation was not rejected by the observed control");
            bool scriptExceptionObserved = false;
            try { await Evaluate(replacement, "throw new Error('context')"); }
            catch (ContextGoneException) { throw new Exception("live script exception misclassified as context removal"); }
            catch (Exception e) when (e.Message.StartsWith("evaluation exception:", StringComparison.Ordinal))
            { Console.WriteLine("LIVE_JS_EXCEPTION_CONTROL " + e.Message); scriptExceptionObserved = true; }
            if (!scriptExceptionObserved) throw new Exception("live-context exception control did not throw");
            const string comparison = "realmProbe('reply-comparison')";
            await Evaluate(replacement, comparison);
            while (!observations.ContainsKey("reply-comparison"))
            { timeout.Token.ThrowIfCancellationRequested(); await Task.Delay(50, timeout.Token); }
            if (observations["reply-comparison"].Unique != replacement.Unique)
                throw new Exception("identical-expression live control reached the wrong context");
            Console.WriteLine("LIVE_IDENTICAL_EVALUATE unique=" + replacement.Unique);
            if (!observations.TryRemove("reply-comparison", out _)) throw new Exception("missing live control observation");
            if (contexts.Values.Any(c => c.Unique == "0.0")) throw new Exception("unobserved-context control is already present");
            Context unobserved = old with { Unique = "0.0" };
            Exception missingError = await RejectedEvaluate(unobserved, comparison);
            Exception oldError = await RejectedEvaluate(old, comparison);
            Console.WriteLine($"UNOBSERVED_EVALUATE_REJECTED type={missingError.GetType().FullName} hresult=0x{missingError.HResult:X8} message={missingError.Message}");
            Console.WriteLine($"DESTROYED_EVALUATE_REJECTED type={oldError.GetType().FullName} hresult=0x{oldError.HResult:X8} message={oldError.Message}");
            if (oldError.GetType() != missingError.GetType() || oldError.HResult != missingError.HResult)
                throw new Exception("destroyed and unobserved controls had different rejection types or codes");
            await Task.Delay(500, timeout.Token);
            if (observations.ContainsKey("reply-comparison") || observations.ContainsKey("early-reply:a:same") ||
                observations.ContainsKey("early-reply:a2:same") || observations.ContainsKey("wrong-object"))
                throw new Exception("rejected operation or replacement object delivered a binding");
            await Task.WhenAll(pauseTasks.ToArray()).WaitAsync(TimeSpan.FromSeconds(3));
            if (eventFailure) throw new Exception("an event, binding or session setup failed");
            Console.WriteLine("EARLY_PLATFORM_PROBE_PASS selected origin-gated exposure and realm observations; OOPIF detach, BFCache, no-script documents and production unproven");
            Result = 0;
        }
        catch (Exception e) { Console.WriteLine("PLATFORM_PROBE_FAILURE " + e); }
        finally { timeout.Cancel(); server.Stop(); Close(); }
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
                if (label.StartsWith("forged:", StringComparison.Ordinal))
                {
                    if (!contexts.TryGetValue((session, id), out var sender) || !sender.Default ||
                        sender.Origin == $"http://127.0.0.1:{port}" ||
                        !rejectedForgeries.TryAdd(label.Substring("forged:".Length), sender))
                        throw new Exception("forged message identity or rejection ambiguous");
                    Console.WriteLine($"FORGERY_REJECTED session={session} context={sender.Unique} origin={sender.Origin}");
                    return;
                }
                if (label.StartsWith("EXPOSURE:", StringComparison.Ordinal))
                {
                    string[] parts = label.Split(':');
                    if (parts.Length != 3 || !exposures.TryAdd(parts[1], parts[2])) throw new Exception("ambiguous exposure observation");
                    label = "EXPOSURE:" + parts[1];
                }
                if (contexts.TryGetValue((session, id), out var c))
                {
                    if (!observations.TryAdd(label, c)) { eventFailure = true; Console.WriteLine("DUPLICATE_BINDING " + label); }
                }
                else { eventFailure = true; Console.WriteLine("UNMATCHED_BINDING " + label); }
            }
            else if (name == "Debugger.scriptParsed")
            {
                if (root.TryGetProperty("executionContextId", out var contextId))
                    scriptContexts[(session, root.GetProperty("scriptId").GetString() ?? "")] = contextId.GetInt32();
            }
            else if (name == "Debugger.paused")
                pauseTasks.Add(HandlePause(session, root.Clone()));
            else if (name == "Debugger.resumed")
            {
                if (!resumeSignals.TryGetValue(session, out var signal) || !signal.Issued)
                    Console.WriteLine("EARLY_UNMATCHED_RESUME session=" + session);
                else if (!signal.Signal.TrySetResult()) throw new Exception("duplicate debugger resume event");
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
            await core.CallDevToolsProtocolMethodForSessionAsync(session, "Debugger.enable", "{}");
            await core.CallDevToolsProtocolMethodForSessionAsync(session, "Debugger.setInstrumentationBreakpoint", "{\"instrumentation\":\"beforeScriptExecution\"}");
            await core.CallDevToolsProtocolMethodForSessionAsync(session, "Target.setAutoAttach", "{\"autoAttach\":true,\"waitForDebuggerOnStart\":true,\"flatten\":true}");
            await core.CallDevToolsProtocolMethodForSessionAsync(session, "Runtime.runIfWaitingForDebugger", "{}");
        }
        catch (Exception e) { eventFailure = true; Console.WriteLine("SESSION_SETUP_FAILURE " + session + " " + e); }
    }

    async Task<string> Call(string session, string method, string parameters)
    {
        Task<string> call = session.Length == 0
            ? web.CoreWebView2.CallDevToolsProtocolMethodAsync(method, parameters)
            : web.CoreWebView2.CallDevToolsProtocolMethodForSessionAsync(session, method, parameters);
        if (method == "Debugger.evaluateOnCallFrame")
        {
            long started = Environment.TickCount64;
            Console.WriteLine($"EARLY_EVAL_BEGIN session={session} tick={started} parameters={parameters}");
            _ = call.ContinueWith(done => Console.WriteLine($"EARLY_EVAL_BACKEND session={session} tick={Environment.TickCount64} elapsed={Environment.TickCount64 - started} status={done.Status}"),
                CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
        }
        return await call.WaitAsync(TimeSpan.FromSeconds(3));
    }

    async Task CallOriginal(Context context, string objectId, string value)
    {
        string args = JsonSerializer.Serialize(new { objectId, functionDeclaration = "function(v){this.onmessage(v)}", arguments = new[] { new { value } }, returnByValue = true });
        string result = await Call(context.Session, "Runtime.callFunctionOn", args);
        using var parsed = JsonDocument.Parse(result);
        if (parsed.RootElement.TryGetProperty("error", out _) || parsed.RootElement.TryGetProperty("exceptionDetails", out _))
            throw new Exception("original-object reply failed: " + result);
    }

    async Task HandlePause(string session, JsonElement paused)
    {
        long started = Stopwatch.GetTimestamp();
        var resumed = new ResumeWait();
        while (!resumeSignals.TryAdd(session, resumed))
        {
            if (!resumeSignals.TryGetValue(session, out var previous)) continue;
            if (previous.Issued && previous.Signal.Task.IsCompletedSuccessfully)
            {
                if (resumeSignals.TryUpdate(session, resumed, previous)) break;
                continue;
            }
            eventFailure = true;
            Console.WriteLine("EARLY_OVERLAPPING_PAUSE session=" + session);
            try { await Call(session, "Debugger.resume", "{}"); }
            catch (Exception e) { Console.WriteLine("EARLY_RESUME_FAILURE " + e); }
            return;
        }
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(3));
        try
        {
            if (paused.GetProperty("reason").GetString() != "instrumentation") throw new Exception("unexpected debugger pause reason");
            var frames = paused.GetProperty("callFrames");
            if (frames.GetArrayLength() == 0) throw new Exception("pause without call frame");
            var frame = frames[0];
            string script = frame.GetProperty("location").GetProperty("scriptId").GetString() ?? "";
            string callFrame = frame.GetProperty("callFrameId").GetString() ?? "";
            if (!scriptContexts.TryGetValue((session, script), out int id) ||
                !contexts.TryGetValue((session, id), out var context) || !context.Default || context.Unique.Length == 0)
                throw new Exception("paused script has no established default realm identity");
            if (initialized.TryAdd(context.Unique, true))
            {
                bool allowed = context.Origin == $"http://127.0.0.1:{port}";
                Console.WriteLine($"EARLY_PAUSE session={session} context={context.Unique} frame={context.Frame} script={script} callFrame={callFrame} origin={context.Origin} allowed={allowed} tick={Environment.TickCount64}");
                if (allowed)
                {
                    string expression = "(()=>{let o={postMessage:function(x){realmProbe('object-call:'+x)}};Object.defineProperty(window,'__web4EarlyObject',{value:o,configurable:true});return o})()";
                    string parameters = JsonSerializer.Serialize(new { callFrameId = callFrame, expression, returnByValue = false });
                    string result = await Call(session, "Debugger.evaluateOnCallFrame", parameters).WaitAsync(deadline.Token);
                    using var parsed = JsonDocument.Parse(result);
                    if (parsed.RootElement.TryGetProperty("error", out _) || parsed.RootElement.TryGetProperty("exceptionDetails", out _))
                        throw new Exception("early injection evaluation failed: " + result);
                    string objectId = parsed.RootElement.GetProperty("result").GetProperty("objectId").GetString() ?? "";
                    if (objectId.Length == 0 || !remoteObjects.TryAdd(context.Unique, objectId))
                        throw new Exception("missing or duplicate injected-object handle");
                }
            }
            deadline.Token.ThrowIfCancellationRequested();
        }
        catch (Exception e) { eventFailure = true; Console.WriteLine($"EARLY_PAUSE_FAILURE elapsed={Stopwatch.GetElapsedTime(started).TotalMilliseconds:F0}ms " + e); }
        finally
        {
            try
            {
                resumed.Issued = true;
                string result = await Call(session, "Debugger.resume", "{}");
                using var parsed = JsonDocument.Parse(result);
                if (parsed.RootElement.TryGetProperty("error", out _)) throw new Exception("resume protocol error: " + result);
                await resumed.Signal.Task.WaitAsync(TimeSpan.FromSeconds(3));
                Console.WriteLine("EARLY_RESUME_OBSERVED session=" + session);
            }
            catch (Exception e) { eventFailure = true; Console.WriteLine("EARLY_RESUME_FAILURE " + e); }
            if (deadline.IsCancellationRequested || Stopwatch.GetElapsedTime(started) >= TimeSpan.FromSeconds(3))
            { eventFailure = true; Console.WriteLine("EARLY_PAUSE_DEADLINE_EXCEEDED"); }
            ((ICollection<KeyValuePair<string, ResumeWait>>)resumeSignals).Remove(new(session, resumed));
        }
    }

    async Task Evaluate(Context context, string expression)
    {
        string parameters = JsonSerializer.Serialize(new { expression, uniqueContextId = context.Unique, returnByValue = true });
        string result;
        try
        {
            result = context.Session.Length == 0
                ? await web.CoreWebView2.CallDevToolsProtocolMethodAsync("Runtime.evaluate", parameters)
                : await web.CoreWebView2.CallDevToolsProtocolMethodForSessionAsync(context.Session, "Runtime.evaluate", parameters);
        }
        catch (Exception e) when (e.Message.Contains("Cannot find context with specified id", StringComparison.OrdinalIgnoreCase))
        { throw new ContextGoneException(e.Message); }
        using var parsed = JsonDocument.Parse(result);
        if (parsed.RootElement.TryGetProperty("error", out var error))
        {
            string detail = error.ToString();
            if (detail.Contains("Cannot find context with specified id", StringComparison.OrdinalIgnoreCase))
                throw new ContextGoneException(detail);
            throw new Exception("CDP error: " + detail);
        }
        if (parsed.RootElement.TryGetProperty("exceptionDetails", out _)) throw new Exception("evaluation exception: " + result);
    }

    async Task<Exception> RejectedEvaluate(Context context, string expression)
    {
        Exception? rejected = null;
        try { await Evaluate(context, expression); }
        catch (ContextGoneException e) { rejected = e; }
        catch (ArgumentException e)
        {
            Console.WriteLine($"EVALUATE_ARGUMENT_ERROR type={e.GetType().FullName} hresult=0x{e.HResult:X8} message={e.Message}");
            if (e.GetType() != typeof(ArgumentException) || e.HResult != unchecked((int)0x80070057)) throw;
            rejected = e;
        }
        return rejected ?? throw new Exception("evaluation unexpectedly succeeded");
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
            var stream = client.GetStream();
            using var reader = new StreamReader(stream, Encoding.ASCII, false, 1024, true);
            string line = await reader.ReadLineAsync() ?? "";
            string path = line.Split(' ').Skip(1).FirstOrDefault() ?? "/";
            string? header;
            while (!string.IsNullOrEmpty(header = await reader.ReadLineAsync())) { }
            if (path.StartsWith("/held")) { holdEntered.TrySetResult(); await holdRelease.Task.WaitAsync(TimeSpan.FromSeconds(30)); }
            string script = "<script>if(window.realmProbe){let n=new URLSearchParams(location.search).get('name')||'main';realmProbe(n);realmProbe('EXPOSURE:'+n+':'+typeof window.__web4EarlyObject);if(window.__web4EarlyObject){window.__web4EarlyObject.onmessage=v=>realmProbe('early-reply:'+n+':'+v);window.__web4EarlyObject.postMessage(n);if(n==='b'){Object.defineProperty(window,'__web4EarlyObject',{value:{onmessage:()=>realmProbe('wrong-object')},configurable:true});realmProbe('replaced-b');}}if(n==='cross'||n==='nestedCross'||n==='sandbox')realmProbe('forged:'+n);}</script>";
            string html = path.StartsWith("/main")
                ? script + $"<iframe id='a' src='/child?name=a'></iframe><iframe src='/child?name=b'></iframe><iframe src='http://localhost:{port}/child?name=cross'></iframe><iframe sandbox='allow-scripts' src='/child?name=sandbox'></iframe>" +
                  "<iframe id='blank'></iframe><iframe srcdoc=\"<script>if(window.realmProbe){realmProbe('srcdoc');realmProbe('EXPOSURE:srcdoc:'+typeof window.__web4EarlyObject);if(window.__web4EarlyObject){window.__web4EarlyObject.onmessage=v=>realmProbe('early-reply:srcdoc:'+v);window.__web4EarlyObject.postMessage('srcdoc')}}</script>\"></iframe>" +
                  "<script>let w=document.getElementById('blank').contentWindow;if(w.realmProbe){w.realmProbe('blank');w.eval(\"realmProbe('EXPOSURE:blank:'+typeof window.__web4EarlyObject);if(window.__web4EarlyObject){window.__web4EarlyObject.onmessage=v=>realmProbe('early-reply:blank:'+v);window.__web4EarlyObject.postMessage('blank')}\")}</script>"
                : script + (path.EndsWith("name=a") ? $"<iframe src='/child?name=nested'></iframe><iframe src='http://localhost:{port}/child?name=nestedCross'></iframe>" : "");
            byte[] body = Encoding.UTF8.GetBytes("<!doctype html>" + html);
            byte[] head = Encoding.ASCII.GetBytes($"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {body.Length}\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n");
            await stream.WriteAsync(head); await stream.WriteAsync(body);
        }
    }
}
