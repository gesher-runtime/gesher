// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Windows' connection cost and data plan change events (Network List Manager,
// INetworkConnectionCostEvents), for the network watcher (windows_networks.cc): the metered
// setting of a connected network changes neither its interface, its addresses nor its routes,
// so Windows' IP change notifications do not report it. A change wakes the watcher, which takes
// a new snapshot, and a callback then hears onCapabilitiesChanged for NET_CAPABILITY_NOT_METERED.
#pragma once

#include <windows.h>

namespace gesher::net {

// INetworkConnectionCostEvents (netlistmgr.h, {DCB0000b-570F-4A9B-8D69-199FDBA5723B}), spelled
// out as mingw-w64's netlistmgr.h does not declare it. The name differs from the SDK's so both
// headers can be included.
extern const IID kIidNetworkConnectionCostEvents;

struct ConnectionCostEvents : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE ConnectionCostChanged(GUID connection_id, DWORD new_cost) = 0;
  virtual HRESULT STDMETHODCALLTYPE ConnectionDataPlanStatusChanged(GUID connection_id) = 0;
};

// A sink that sets `event` (not owned; it must outlive the sink) for either notification.
// One reference.
ConnectionCostEvents* NewConnectionCostSink(HANDLE event);

struct CostWatch;

// Subscribes such a sink to the Network List Manager (the cost manager's connection point, as
// Microsoft's NetworkCost sample does) on a thread of its own, which stays in the multithreaded
// apartment while subscribed. Returns null and the reason in `hr` when Windows refuses (no
// Network List Manager, no such connection point); the caller keeps polling then.
// Owns a duplicate event handle so a timed-out initialization can finish safely after return.
CostWatch* StartCostWatch(HANDLE event, HRESULT* hr);

// Unsubscribes and ends the thread. Null is fine.
void StopCostWatch(CostWatch* watch);

}  // namespace gesher::net
