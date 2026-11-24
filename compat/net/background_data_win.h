// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Whether Windows restricts background data on the internet connection
// (Windows.Networking.Connectivity: NetworkInformation.GetInternetConnectionProfile()
// .GetConnectionCost().BackgroundDataUsageRestricted), for the network snapshot's
// "background" line (windows_networks.cc) and so ConnectivityManager's
// getRestrictBackgroundStatus.
#pragma once

#include <windows.h>

namespace gesher::net {

// 1: restricted; 0: not restricted; -1: unknown (no internet connection profile, no
// WinRT, an older Windows without IConnectionCost2, or a failure; `hr` says which).
int BackgroundDataRestricted(HRESULT* hr);

}  // namespace gesher::net
