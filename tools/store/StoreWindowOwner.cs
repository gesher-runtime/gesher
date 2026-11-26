// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Runtime.InteropServices;
using System.Threading;

namespace Gesher.Store
{
    public static class StoreWindowOwner
    {
        [ComImport, Guid("3E68D4BD-7135-4D10-8018-9FB6D9F33FA1")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IInitializeWithWindow
        {
            void Initialize(IntPtr owner);
        }

        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);

        [DllImport("kernel32.dll")]
        private static extern uint GetCurrentThreadId();

        [DllImport("kernel32.dll")]
        private static extern uint GetCurrentProcessId();

        public static void Attach(object context, IntPtr owner)
        {
            uint process;
            uint thread = GetWindowThreadProcessId(owner, out process);
            if (thread == 0 || thread != GetCurrentThreadId() || process != GetCurrentProcessId() ||
                Thread.CurrentThread.GetApartmentState() != ApartmentState.STA)
                throw new ArgumentException("Purchase requires an owner on the current desktop UI thread.", "owner");

            ((IInitializeWithWindow)context).Initialize(owner);
        }
    }
}
