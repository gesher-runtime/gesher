// CI-only WebView2/CDP capability experiment. Not the Gesher bridge.
using System.Collections.Concurrent;
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
            web.Source = new Uri($"http://127.0.0.1:{port}/main");
            string[] expected = { "main", "a", "b", "nested", "cross", "nestedCross", "sandbox", "blank", "srcdoc" };
            while (!expected.All(observations.ContainsKey))
            {
                timeout.Token.ThrowIfCancellationRequested();
                await Task.Delay(50, timeout.Token);
            }
            foreach (string label in expected)
            {
                Context c = observations[label];
                if (!c.Default || c.Unique.Length == 0 || c.Frame.Length == 0) throw new Exception("missing default context identity for " + label);
                Console.WriteLine($"OBS {label} session={c.Session} id={c.Id} unique={c.Unique} origin={c.Origin} frame={c.Frame}");
            }
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
            if (observations.ContainsKey("reply-comparison")) throw new Exception("rejected evaluation delivered a binding");
            if (eventFailure) throw new Exception("an event, binding or session setup failed");
            Console.WriteLine("PLATFORM_PROBE_PASS selected contexts, origins, binding callers, held navigation and rejected old-context operation; rejection reason and OOPIF detach/stale lifecycle unproven");
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
            string script = "<script>if(window.realmProbe)realmProbe(new URLSearchParams(location.search).get('name')||'main');</script>";
            string html = path.StartsWith("/main")
                ? script + $"<iframe id='a' src='/child?name=a'></iframe><iframe src='/child?name=b'></iframe><iframe src='http://localhost:{port}/child?name=cross'></iframe><iframe sandbox='allow-scripts' src='/child?name=sandbox'></iframe>" +
                  "<iframe id='blank'></iframe><iframe srcdoc=\"<script>if(window.realmProbe)realmProbe('srcdoc')</script>\"></iframe>" +
                  "<script>let w=document.getElementById('blank').contentWindow;if(w.realmProbe)w.realmProbe('blank')</script>"
                : script + (path.EndsWith("name=a") ? $"<iframe src='/child?name=nested'></iframe><iframe src='http://localhost:{port}/child?name=nestedCross'></iframe>" : "");
            byte[] body = Encoding.UTF8.GetBytes("<!doctype html>" + html);
            byte[] head = Encoding.ASCII.GetBytes($"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {body.Length}\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n");
            await stream.WriteAsync(head); await stream.WriteAsync(body);
        }
    }
}
