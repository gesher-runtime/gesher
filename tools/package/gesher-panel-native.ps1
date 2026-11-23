# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Compile with the panel's existing taskbar helper, not in a second compiler call.
if (-not ('Gesher.PanelChangeMonitor' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

namespace Gesher {
  public static class Win32ShellGroup {
    [DllImport("shell32.dll", CharSet=CharSet.Unicode)]
    public static extern int SetCurrentProcessExplicitAppUserModelID(string appID);
  }

  // Native/.NET notification callbacks only mark state and post a managed delegate.
  // PowerShell subscribers run on the supplied WPF DispatcherSynchronizationContext.
  public sealed class PanelChangeMonitor : IDisposable {
    public const int MaximumWatchers = 8;
    readonly object gate = new object();
    readonly SynchronizationContext context;
    readonly int ownerThread;
    readonly List<FileSystemWatcher> watchers = new List<FileSystemWatcher>();
    HashSet<string> files = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    HashSet<string> directories = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    HashSet<string> configured = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    string root = "";
    bool disposed, queued, faulted;
    long generation;
    Process process;
    public event EventHandler Changed;

    public PanelChangeMonitor(SynchronizationContext dispatcherContext) {
      if (dispatcherContext == null || dispatcherContext.GetType() == typeof(SynchronizationContext))
        throw new ArgumentException("A dispatcher synchronization context is required");
      context = dispatcherContext;
      ownerThread = Thread.CurrentThread.ManagedThreadId;
    }
    public long Generation { get { return Interlocked.Read(ref generation); } }
    public bool Healthy { get { lock (gate) { return !disposed && !faulted; } } }
    public int WatcherCount { get { RequireOwner(); return watchers.Count; } }
    public int ProcessCount { get { RequireOwner(); return process == null ? 0 : 1; } }
    void RequireOwner() {
      if (Thread.CurrentThread.ManagedThreadId != ownerThread)
        throw new InvalidOperationException("Panel monitor ownership crossed the dispatcher thread");
    }
    static string Full(string path) {
      string value = Path.GetFullPath(path);
      string pathRoot = Path.GetPathRoot(value);
      return value.Length > pathRoot.Length ? value.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) : value;
    }
    static bool Within(string path, string parent) {
      string prefix = parent.EndsWith(Path.DirectorySeparatorChar.ToString(), StringComparison.Ordinal)
          ? parent : parent + Path.DirectorySeparatorChar;
      return String.Equals(path, parent, StringComparison.OrdinalIgnoreCase) ||
          path.StartsWith(prefix, StringComparison.OrdinalIgnoreCase);
    }
    static string ExistingDirectory(string path) {
      while (!String.IsNullOrEmpty(path) && !Directory.Exists(path)) path = Path.GetDirectoryName(path);
      return path;
    }
    void Signal() {
      lock (gate) {
        if (disposed) return;
        Interlocked.Increment(ref generation);
        if (queued) return;
        queued = true;
      }
      try { context.Post(delegate(object unused) { Deliver(); }, null); }
      catch { lock (gate) { queued = false; faulted = true; } }
    }
    void Deliver() {
      EventHandler handler;
      lock (gate) {
        queued = false;
        if (disposed) return;
        // Never invoke a PowerShell delegate on a pool thread, even if a caller
        // supplies a broken synchronization context. Visible recovery still runs.
        if (Thread.CurrentThread.ManagedThreadId != ownerThread) { faulted = true; return; }
        handler = Changed;
      }
      if (handler != null) handler(this, EventArgs.Empty);
    }
    bool Relevant(string path) {
      lock (gate) {
        if (disposed) return false;
        if (files.Contains(path) || directories.Contains(path)) return true;
        // Install writes the new app's shortcut after packages.list, possibly
        // before the UI has discovered that app and added its exact icon paths.
        string apps = Path.Combine(root, "data", "data");
        if (!Within(path, apps)) return false;
        string parent = Path.GetDirectoryName(path);
        string name = Path.GetFileName(path);
        return String.Equals(Path.GetFileName(parent), "gesher-shell", StringComparison.OrdinalIgnoreCase) &&
            (String.Equals(name, "shortcut.properties", StringComparison.OrdinalIgnoreCase) ||
             String.Equals(name, "icon.ico", StringComparison.OrdinalIgnoreCase));
      }
    }
    void OnFile(object sender, FileSystemEventArgs args) { if (Relevant(args.FullPath)) Signal(); }
    void OnRename(object sender, RenamedEventArgs args) {
      if (Relevant(args.FullPath) || Relevant(args.OldFullPath)) Signal();
    }
    void OnError(object sender, ErrorEventArgs args) {
      bool first;
      lock (gate) { first = !faulted; faulted = true; }
      if (first) Signal();
    }
    void OnExited(object sender, EventArgs args) { Signal(); }

    public void SetFiles(string dataRoot, string[] paths) {
      RequireOwner();
      lock (gate) { if (disposed) return; }
      string newRoot = Full(dataRoot);
      HashSet<string> newFiles = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
      HashSet<string> newDirectories = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
      List<KeyValuePair<string, bool>> roots = new List<KeyValuePair<string, bool>>();
      HashSet<string> keys = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
      string rootDirectory = ExistingDirectory(newRoot);
      if (!String.IsNullOrEmpty(rootDirectory)) {
        bool recursive = String.Equals(rootDirectory, newRoot, StringComparison.OrdinalIgnoreCase);
        roots.Add(new KeyValuePair<string, bool>(rootDirectory, recursive));
        keys.Add(rootDirectory + "|" + recursive);
      }
      foreach (string path in paths) {
        if (String.IsNullOrWhiteSpace(path)) continue;
        string full;
        try { full = Full(path); } catch { continue; }
        newFiles.Add(full);
        string parent = Path.GetDirectoryName(full);
        for (string at = parent; !String.IsNullOrEmpty(at); at = Path.GetDirectoryName(at))
          newDirectories.Add(at);
        if (Within(full, newRoot) || roots.Count >= MaximumWatchers) continue;
        string directory = ExistingDirectory(parent);
        if (!String.IsNullOrEmpty(directory) && keys.Add(directory + "|False"))
          roots.Add(new KeyValuePair<string, bool>(directory, false));
      }
      for (string at = newRoot; !String.IsNullOrEmpty(at); at = Path.GetDirectoryName(at))
        newDirectories.Add(at);
      bool rebuild;
      lock (gate) {
        root = newRoot; files = newFiles; directories = newDirectories;
        rebuild = faulted || !configured.SetEquals(keys);
        if (rebuild) faulted = false;
      }
      if (!rebuild) return;
      CloseWatchers();
      bool healthy = true;
      foreach (KeyValuePair<string, bool> entry in roots) {
        FileSystemWatcher watcher = null;
        try {
          watcher = new FileSystemWatcher(entry.Key);
          watcher.IncludeSubdirectories = entry.Value;
          watcher.NotifyFilter = NotifyFilters.FileName | NotifyFilters.DirectoryName | NotifyFilters.LastWrite | NotifyFilters.Size;
          watcher.InternalBufferSize = 8192;
          watcher.Changed += OnFile; watcher.Created += OnFile; watcher.Deleted += OnFile;
          watcher.Renamed += OnRename; watcher.Error += OnError;
          watcher.EnableRaisingEvents = true;
          watchers.Add(watcher);
        } catch { healthy = false; if (watcher != null) watcher.Dispose(); }
      }
      // An asynchronous watcher error during setup must not be overwritten by
      // the successful creation of the remaining watchers.
      lock (gate) { configured = keys; faulted = faulted || !healthy; healthy = !faulted; }
      // Replacing a watch has a gap. A follow-up UI read covers it, without
      // claiming the freshly read view already includes every intervening event.
      if (healthy) Signal();
    }

    public void SetProcess(int pid) {
      RequireOwner();
      lock (gate) { if (disposed) return; }
      try { if (process != null && process.Id == pid && !process.HasExited) return; }
      catch { }
      CloseProcess();
      if (pid <= 0) return;
      Process next = null;
      try {
        next = Process.GetProcessById(pid);
        next.Exited += OnExited;
        next.EnableRaisingEvents = true;
        bool exited = next.HasExited;
        process = next;
        next = null;
        if (exited) Signal();
      } catch { if (next != null) next.Dispose(); Signal(); }
    }
    void CloseProcess() {
      if (process == null) return;
      process.Exited -= OnExited;
      process.Dispose();
      process = null;
    }
    void CloseWatchers() {
      foreach (FileSystemWatcher watcher in watchers) watcher.Dispose();
      watchers.Clear();
    }
    public void Dispose() {
      RequireOwner();
      lock (gate) { if (disposed) return; disposed = true; Changed = null; }
      CloseWatchers();
      CloseProcess();
    }
  }
}
'@
}
