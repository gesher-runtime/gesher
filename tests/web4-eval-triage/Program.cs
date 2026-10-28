// CI-only minimal WebView2/CDP paused-call-frame diagnostic. No guest injection.
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
    [STAThread] static int Main()
    {
        ApplicationConfiguration.Initialize();
        using var form = new TriageForm();
        Application.Run(form);
        return form.Result;
    }
}

sealed class TriageForm : Form
{
    sealed class Context { public string Unique = ""; public string Frame = ""; public bool Default; }
    sealed class PauseState { public bool ResumeIssued; public bool Premature; public readonly TaskCompletionSource Resumed = new(TaskCreationOptions.RunContinuationsAsynchronously); }
    readonly WebView2 web = new() { Dock = DockStyle.Fill };
    readonly List<CoreWebView2DevToolsProtocolEventReceiver> receivers = new();
    readonly ConcurrentDictionary<(string, int), Context> contexts = new();
    readonly ConcurrentDictionary<(string, string), int> scripts = new();
    readonly ConcurrentDictionary<string, string> targetTypes = new();
    readonly ConcurrentDictionary<string, PauseState> pauses = new();
    readonly ConcurrentDictionary<string, bool> attempts = new();
    readonly ConcurrentDictionary<string, bool> results = new();
    readonly TcpListener server = new(IPAddress.Any, 0);
    bool failure;
    int port;
    public int Result { get; private set; } = 1;

    public TriageForm() { Controls.Add(web); Shown += async (_, _) => await Run(); }

    async Task Run()
    {
        using var limit = new CancellationTokenSource(TimeSpan.FromSeconds(60));
        try
        {
            server.Start(); port = ((IPEndPoint)server.LocalEndpoint).Port; _ = Serve(limit.Token);
            var env = await CoreWebView2Environment.CreateAsync(null, Path.Combine(Path.GetTempPath(), "web4-eval-" + Guid.NewGuid()));
            await web.EnsureCoreWebView2Async(env);
            Console.WriteLine("RUNTIME " + env.BrowserVersionString);
            foreach (string name in new[] { "Target.attachedToTarget", "Runtime.executionContextCreated", "Runtime.executionContextDestroyed", "Runtime.executionContextsCleared", "Debugger.scriptParsed", "Debugger.paused", "Debugger.resumed" })
            {
                var receiver = web.CoreWebView2.GetDevToolsProtocolEventReceiver(name);
                receiver.DevToolsProtocolEventReceived += (_, e) => Event(name, e.SessionId ?? "", e.ParameterObjectAsJson);
                receivers.Add(receiver);
            }
            await Setup("");
            await Command("", "Target.setAutoAttach", "{\"autoAttach\":true,\"waitForDebuggerOnStart\":true,\"flatten\":true}");
            web.Source = new Uri($"http://127.0.0.1:{port}/main");
            while (!results.ContainsKey("root") || !results.ContainsKey("iframe"))
            { limit.Token.ThrowIfCancellationRequested(); await Task.Delay(50, limit.Token); }
            if (failure || !results["root"] || !results["iframe"]) throw new Exception("at least one selected paused-eval triage failed");
            Console.WriteLine("EVAL_TRIAGE_PASS selected default-target and actual iframe-target paused expression 1, live nonpaused controls");
            Result = 0;
        }
        catch (Exception e) { Console.WriteLine("EVAL_TRIAGE_FAILURE " + e); }
        finally { limit.Cancel(); server.Stop(); Close(); }
    }

    async Task<string> Command(string session, string method, string parameters)
    {
        Task<string> task = session.Length == 0
            ? web.CoreWebView2.CallDevToolsProtocolMethodAsync(method, parameters)
            : web.CoreWebView2.CallDevToolsProtocolMethodForSessionAsync(session, method, parameters);
        return await task.WaitAsync(TimeSpan.FromSeconds(3));
    }

    async Task Setup(string session)
    {
        await Command(session, "Runtime.enable", "{}");
        await Command(session, "Debugger.enable", "{}");
        await Command(session, "Debugger.setInstrumentationBreakpoint", "{\"instrumentation\":\"beforeScriptExecution\"}");
    }

    async Task SetupTarget(string session)
    {
        try
        {
            await Setup(session);
            await Command(session, "Target.setAutoAttach", "{\"autoAttach\":true,\"waitForDebuggerOnStart\":true,\"flatten\":true}");
            await Command(session, "Runtime.runIfWaitingForDebugger", "{}");
        }
        catch (Exception e) { failure = true; Console.WriteLine("TARGET_SETUP_FAILURE " + e); }
    }

    void Event(string name, string session, string json)
    {
        Console.WriteLine("CDP " + name + " session=" + session + " " + json);
        try
        {
            using var doc = JsonDocument.Parse(json); var root = doc.RootElement;
            if (name == "Target.attachedToTarget")
            {
                string child = root.GetProperty("sessionId").GetString() ?? "";
                targetTypes[child] = root.GetProperty("targetInfo").GetProperty("type").GetString() ?? "";
                _ = SetupTarget(child);
            }
            else if (name == "Runtime.executionContextCreated")
            {
                var c = root.GetProperty("context"); var aux = c.GetProperty("auxData");
                contexts[(session, c.GetProperty("id").GetInt32())] = new Context {
                    Unique = c.GetProperty("uniqueId").GetString() ?? "", Frame = aux.GetProperty("frameId").GetString() ?? "",
                    Default = aux.GetProperty("isDefault").GetBoolean() };
            }
            else if (name == "Runtime.executionContextDestroyed")
            {
                if (root.TryGetProperty("executionContextId", out var id)) contexts.TryRemove((session, id.GetInt32()), out _);
            }
            else if (name == "Runtime.executionContextsCleared")
                foreach (var key in contexts.Keys.Where(k => k.Item1 == session)) contexts.TryRemove(key, out _);
            else if (name == "Debugger.scriptParsed")
            {
                if (root.TryGetProperty("executionContextId", out var id)) scripts[(session, root.GetProperty("scriptId").GetString() ?? "")] = id.GetInt32();
            }
            else if (name == "Debugger.paused") _ = HandlePause(session, root.Clone());
            else if (name == "Debugger.resumed" && pauses.TryGetValue(session, out var state))
            {
                if (!state.ResumeIssued) { state.Premature = true; failure = true; Console.WriteLine("PREMATURE_RESUME session=" + session); }
                else state.Resumed.TrySetResult();
            }
        }
        catch (Exception e) { failure = true; Console.WriteLine("EVENT_FAILURE " + e); }
    }

    async Task HandlePause(string session, JsonElement paused)
    {
        string role = session.Length == 0 ? "root" : targetTypes.TryGetValue(session, out var t) && t == "iframe" ? "iframe" : "other";
        bool selected = role != "other" && attempts.TryAdd(role, true);
        if (!selected)
        {
            if (pauses.ContainsKey(session))
            {
                failure = true;
                Console.WriteLine("OVERLAPPING_PAUSE_WITHOUT_EARLY_RESUME session=" + session);
                return;
            }
            try { await Command(session, "Debugger.resume", "{}"); } catch (Exception e) { failure = true; Console.WriteLine("EXTRA_RESUME_FAILURE " + e); }
            return;
        }
        long start = Environment.TickCount64, deadline = start + 3000;
        var state = new PauseState();
        if (!pauses.TryAdd(session, state)) { failure = true; Console.WriteLine("AMBIGUOUS_PAUSE " + session); return; }
        bool pausedEval = false, liveEval = false;
        Context? context = null;
        try
        {
            if (paused.GetProperty("reason").GetString() != "instrumentation") throw new Exception("unexpected pause reason");
            var frame = paused.GetProperty("callFrames")[0];
            string script = frame.GetProperty("location").GetProperty("scriptId").GetString() ?? "";
            string callFrame = frame.GetProperty("callFrameId").GetString() ?? "";
            if (!scripts.TryGetValue((session, script), out int id) || !contexts.TryGetValue((session, id), out context) ||
                !context.Default || context.Unique.Length == 0 || context.Frame.Length == 0 || callFrame.Length == 0)
                throw new Exception("missing engine context/frame/callFrame identity");
            string parameters = JsonSerializer.Serialize(new { callFrameId = callFrame, expression = "1", returnByValue = true });
            Console.WriteLine($"PAUSED_EVAL_SUBMIT role={role} session={session} context={context.Unique} frame={context.Frame} script={script} tick={Environment.TickCount64} deadline={deadline} params={parameters}");
            if (Environment.TickCount64 >= deadline) throw new TimeoutException("deadline expired before paused evaluation submission");
            Task<string> backend = session.Length == 0
                ? web.CoreWebView2.CallDevToolsProtocolMethodAsync("Debugger.evaluateOnCallFrame", parameters)
                : web.CoreWebView2.CallDevToolsProtocolMethodForSessionAsync(session, "Debugger.evaluateOnCallFrame", parameters);
            _ = backend.ContinueWith(done => {
                Console.WriteLine($"PAUSED_EVAL_BACKEND_OBSERVED role={role} session={session} tick={Environment.TickCount64} status={done.Status}");
                if (done.Exception != null)
                {
                    for (Exception? e = done.Exception; e != null; e = e.InnerException)
                        Console.WriteLine($"PAUSED_EVAL_BACKEND_FAULT type={e.GetType().FullName} hresult=0x{e.HResult:X8} message={e.Message}");
                    Console.WriteLine($"PAUSED_EVAL_BACKEND_CHAIN {done.Exception}");
                }
            }, CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
            int remaining = (int)Math.Max(1, deadline - Environment.TickCount64);
            string result = await backend.WaitAsync(TimeSpan.FromMilliseconds(remaining));
            long observed = Environment.TickCount64;
            if (state.Premature || state.ResumeIssued || observed >= deadline) throw new Exception("resume or outcome outside paused deadline");
            using var parsed = JsonDocument.Parse(result);
            if (parsed.RootElement.TryGetProperty("exceptionDetails", out _) || parsed.RootElement.TryGetProperty("error", out _) ||
                parsed.RootElement.GetProperty("result").GetProperty("value").GetInt32() != 1) throw new Exception("paused expression did not return 1: " + result);
            Console.WriteLine($"PAUSED_EVAL_ONE role={role} session={session} context={context.Unique} tick={observed} result={result}");
            pausedEval = true;
        }
        catch (Exception e) { Console.WriteLine($"PAUSED_EVAL_FAILURE role={role} tick={Environment.TickCount64} type={e.GetType().FullName} hresult=0x{e.HResult:X8} chain={e}"); }
        finally
        {
            try
            {
                state.ResumeIssued = true;
                Console.WriteLine($"RESUME_SUBMIT role={role} session={session} tick={Environment.TickCount64}");
                await Command(session, "Debugger.resume", "{}");
                await state.Resumed.Task.WaitAsync(TimeSpan.FromSeconds(3));
                Console.WriteLine($"RESUME_OBSERVED role={role} tick={Environment.TickCount64}");
                if (Environment.TickCount64 >= deadline || state.Premature) pausedEval = false;
                await Command(session, "Debugger.setBreakpointsActive", "{\"active\":false}");
                if (context != null)
                {
                    string parameters = JsonSerializer.Serialize(new { expression = "1", uniqueContextId = context.Unique, returnByValue = true });
                    string result = await Command(session, "Runtime.evaluate", parameters);
                    using var parsed = JsonDocument.Parse(result);
                    liveEval = !parsed.RootElement.TryGetProperty("error", out _) && !parsed.RootElement.TryGetProperty("exceptionDetails", out _) && parsed.RootElement.GetProperty("result").GetProperty("value").GetInt32() == 1;
                    Console.WriteLine($"LIVE_EVAL_ONE role={role} session={session} context={context.Unique} result={result}");
                }
            }
            catch (Exception e) { Console.WriteLine($"RESUME_OR_LIVE_FAILURE role={role} type={e.GetType().FullName} hresult=0x{e.HResult:X8} chain={e}"); }
            pauses.TryRemove(session, out _);
            results[role] = pausedEval && liveEval && !state.Premature;
            Console.WriteLine($"TRIAGE_RESULT role={role} paused={pausedEval} live={liveEval} prematureResume={state.Premature}");
        }
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
            string html = path.StartsWith("/main") ? $"<script>window.fixture=1</script><iframe src='http://localhost:{port}/child'></iframe>" : "<script>window.fixture=1</script>";
            byte[] body = Encoding.UTF8.GetBytes("<!doctype html>" + html);
            byte[] head = Encoding.ASCII.GetBytes($"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: {body.Length}\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n");
            await stream.WriteAsync(head); await stream.WriteAsync(body);
        }
    }
}
