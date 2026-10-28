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
    readonly WebView2 web = new() { Dock = DockStyle.Fill };
    readonly ConcurrentDictionary<(string, int), Context> contexts = new();
    readonly ConcurrentDictionary<string, Context> observations = new();
    readonly ConcurrentDictionary<string, bool> destroyed = new();
    readonly ConcurrentDictionary<string, Task> sessionSetup = new();
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
            foreach (string name in new[] { "Runtime.executionContextCreated", "Runtime.executionContextDestroyed", "Runtime.executionContextsCleared", "Runtime.bindingCalled", "Target.attachedToTarget", "Target.detachedFromTarget" })
            {
                var receiver = core.GetDevToolsProtocolEventReceiver(name);
                receiver.DevToolsProtocolEventReceived += (_, e) => Event(name, e.SessionId ?? "", e.ParameterObjectAsJson);
            }
            await core.CallDevToolsProtocolMethodAsync("Runtime.enable", "{}");
            await core.CallDevToolsProtocolMethodAsync("Runtime.addBinding", "{\"name\":\"realmProbe\"}");
            await core.CallDevToolsProtocolMethodAsync("Target.setAutoAttach", "{\"autoAttach\":true,\"waitForDebuggerOnStart\":true,\"flatten\":true}");
            web.Source = new Uri($"http://127.0.0.1:{port}/main");
            string[] expected = { "main", "a", "b", "nested", "cross", "sandbox", "blank", "srcdoc" };
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
            if (observations["cross"].Session.Length == 0) throw new Exception("cross-origin target session was not observed");
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
            holdRelease.TrySetResult();
            while (!observations.ContainsKey("a2") || !destroyed.ContainsKey(old.Unique))
            {
                timeout.Token.ThrowIfCancellationRequested(); await Task.Delay(50, timeout.Token);
            }
            bool staleRejected = false;
            try { await Evaluate(old, "realmProbe('stale-reply')"); }
            catch (Exception e) when (e.Message.Contains("context", StringComparison.OrdinalIgnoreCase))
            { Console.WriteLine("STALE_CONTEXT_REJECTION " + e.Message); staleRejected = true; }
            await Task.Delay(500, timeout.Token);
            if (!staleRejected || observations.ContainsKey("stale-reply")) throw new Exception("stale context accepted reply");
            if (eventFailure) throw new Exception("an event, binding or session setup failed");
            Console.WriteLine("PLATFORM_PROBE_PASS contexts, origins, binding callers, held navigation, unique-context replies and stale rejection");
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
            if (name == "Target.attachedToTarget")
            {
                string child = root.GetProperty("sessionId").GetString() ?? "";
                sessionSetup[child] = SetupSession(child);
            }
            else if (name == "Runtime.executionContextCreated")
            {
                var c = root.GetProperty("context");
                var aux = c.TryGetProperty("auxData", out var a) ? a : default;
                int id = c.GetProperty("id").GetInt32();
                string unique = c.TryGetProperty("uniqueId", out var u) ? u.GetString() ?? "" : "";
                string origin = c.TryGetProperty("origin", out var o) ? o.GetString() ?? "" : "";
                string frame = aux.ValueKind == JsonValueKind.Object && aux.TryGetProperty("frameId", out var f) ? f.GetString() ?? "" : "";
                bool main = aux.ValueKind == JsonValueKind.Object && aux.TryGetProperty("isDefault", out var d) && d.ValueKind == JsonValueKind.True;
                contexts[(session, id)] = new Context(session, id, unique, origin, frame, main);
            }
            else if (name == "Runtime.bindingCalled")
            {
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
            await core.CallDevToolsProtocolMethodForSessionAsync(session, "Runtime.addBinding", "{\"name\":\"realmProbe\"}");
            await core.CallDevToolsProtocolMethodForSessionAsync(session, "Runtime.runIfWaitingForDebugger", "{}");
        }
        catch (Exception e) { eventFailure = true; Console.WriteLine("SESSION_SETUP_FAILURE " + session + " " + e); }
    }

    async Task Evaluate(Context context, string expression)
    {
        string parameters = JsonSerializer.Serialize(new { expression, uniqueContextId = context.Unique, returnByValue = true });
        string result = context.Session.Length == 0
            ? await web.CoreWebView2.CallDevToolsProtocolMethodAsync("Runtime.evaluate", parameters)
            : await web.CoreWebView2.CallDevToolsProtocolMethodForSessionAsync(context.Session, "Runtime.evaluate", parameters);
        using var parsed = JsonDocument.Parse(result);
        if (parsed.RootElement.TryGetProperty("error", out var error)) throw new Exception("CDP error: " + error);
        if (parsed.RootElement.TryGetProperty("exceptionDetails", out _)) throw new Exception("evaluation exception: " + result);
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
                : script + (path.EndsWith("name=a") ? "<iframe src='/child?name=nested'></iframe>" : "");
            byte[] body = Encoding.UTF8.GetBytes("<!doctype html>" + html);
            byte[] head = Encoding.ASCII.GetBytes($"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {body.Length}\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n");
            await stream.WriteAsync(head); await stream.WriteAsync(body);
        }
    }
}
