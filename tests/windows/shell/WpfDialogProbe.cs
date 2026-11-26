// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Drive real production dialogs without nesting a PowerShell timer callback
// around the production PowerShell Click callback.
using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Automation.Peers;
using System.Windows.Automation.Provider;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Threading;

namespace Gesher.Tests {
    public sealed class WpfDialogProbe : IDisposable {
        private readonly Window owner;
        private readonly string action;
        private readonly bool messageDialog;
        private readonly DispatcherTimer timer;
        private DateTime deadline;

        public bool Observed { get; private set; }
        public string Failure { get; private set; }
        public int VisibleWindowCount { get; private set; }
        public bool OwnerMatches { get; private set; }
        public string FlowDirection { get; private set; }
        public string Title { get; private set; }
        public List<string> Texts { get; private set; }
        public string CancelText { get; private set; }
        public bool CancelIsCancel { get; private set; }
        public bool CancelIsDefault { get; private set; }
        public bool CancelHasFocus { get; private set; }
        public string ConfirmVisibility { get; private set; }
        public bool ConfirmIsEnabled { get; private set; }
        public bool ConfirmIsDefault { get; private set; }
        public string OkText { get; private set; }
        public bool OkIsCancel { get; private set; }

        public WpfDialogProbe(Window owner, string action, bool messageDialog) {
            this.owner = owner;
            this.action = action;
            this.messageDialog = messageDialog;
            Texts = new List<string>();
            // Inspect after the production ContentRendered handler has set initial focus.
            timer = new DispatcherTimer(DispatcherPriority.ApplicationIdle, owner.Dispatcher);
            timer.Interval = TimeSpan.FromMilliseconds(20);
            timer.Tick += Tick;
        }

        public void Start() {
            deadline = DateTime.UtcNow.AddSeconds(10);
            timer.Start();
        }

        private void Tick(object sender, EventArgs args) {
            try {
                if (DateTime.UtcNow > deadline) {
                    FailAndClose("Production modal did not close within 10 seconds");
                    return;
                }
                if (Observed) return;
                Window modal = null;
                foreach (Window candidate in owner.OwnedWindows) {
                    if (!candidate.IsVisible) continue;
                    VisibleWindowCount++;
                    if (modal == null) modal = candidate;
                }
                if (modal == null) return;
                Observed = true;
                OwnerMatches = Object.ReferenceEquals(modal.Owner, owner);
                FlowDirection = modal.FlowDirection.ToString();
                Title = modal.Title;
                CollectText(modal);

                Button button;
                if (messageDialog) {
                    button = modal.FindName("BtnOk") as Button;
                    if (button == null) throw new InvalidOperationException("Message dialog has no OK button");
                    OkText = Convert.ToString(button.Content);
                    OkIsCancel = button.IsCancel;
                } else {
                    Button cancel = modal.FindName("BtnCancel") as Button;
                    if (cancel == null) throw new InvalidOperationException("Confirmation has no cancel button");
                    CancelText = Convert.ToString(cancel.Content);
                    CancelIsCancel = cancel.IsCancel;
                    CancelIsDefault = cancel.IsDefault;
                    CancelHasFocus = Object.ReferenceEquals(FocusManager.GetFocusedElement(modal), cancel);
                    button = modal.FindName("BtnConfirm") as Button;
                    if (button == null) throw new InvalidOperationException("Confirmation has no confirm button");
                    ConfirmVisibility = button.Visibility.ToString();
                    ConfirmIsEnabled = button.IsEnabled;
                    ConfirmIsDefault = button.IsDefault;
                }

                if (action == "Close") {
                    modal.Close();
                } else if (action == "Enter" || action == "Escape") {
                    // Exercise WPF's registered default/cancel key route, including
                    // Button.OnClick and IsCancel, without injecting native keyboard input.
                    string key = action == "Enter" ? "\r" : "\x1b";
                    PresentationSource scope = PresentationSource.FromVisual(modal);
                    if (scope == null || !AccessKeyManager.IsKeyRegistered(scope, key)) {
                        throw new InvalidOperationException("Modal has no registered " + action + " action");
                    }
                    AccessKeyManager.ProcessKey(scope, key, false);
                } else if (action == "Cancel") {
                    // Invoke OnClick, including WPF's IsCancel behavior.
                    Button cancel = (Button)modal.FindName("BtnCancel");
                    IInvokeProvider provider = (IInvokeProvider)new ButtonAutomationPeer(cancel)
                        .GetPattern(PatternInterface.Invoke);
                    provider.Invoke();
                } else {
                    // This invokes the real production PowerShell Click handler.
                    button.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                }
            } catch (Exception error) {
                FailAndClose(error.ToString());
            }
        }

        private void CollectText(DependencyObject node) {
            TextBlock text = node as TextBlock;
            if (text != null) Texts.Add(text.Text);
            foreach (object child in LogicalTreeHelper.GetChildren(node)) {
                DependencyObject element = child as DependencyObject;
                if (element != null) CollectText(element);
            }
        }

        private void FailAndClose(string error) {
            Failure = error;
            timer.Stop();
            CloseDialogs();
        }

        private void CloseDialogs() {
            // This test process owns only its fixture owner and test dialogs.
            // Also close a dialog with the wrong owner so that error is bounded.
            List<Window> windows = new List<Window>();
            foreach (PresentationSource source in PresentationSource.CurrentSources) {
                Window window = source.RootVisual as Window;
                if (window != null && !Object.ReferenceEquals(window, owner)) windows.Add(window);
            }
            foreach (Window window in windows) {
                try { window.Close(); }
                catch (Exception error) {
                    if (Failure == null) Failure = error.ToString();
                }
            }
        }

        public void Dispose() {
            timer.Stop();
            timer.Tick -= Tick;
            CloseDialogs();
        }
    }
}
