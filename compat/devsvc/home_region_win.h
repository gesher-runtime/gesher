// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// The user's Windows "Country or region" (Settings > Time & language > Language & region),
// for com.gesher.server.CountryDetectorService through WindowsHost.homeRegion.
#pragma once

namespace dev {

// Writes the region as two upper-case ASCII letters (ISO 3166-1 alpha-2) and a NUL into
// `out` and returns true. GetUserDefaultGeoName (Windows 10 1709 and later), else
// GetUserGeoID(GEOCLASS_NATION) with GetGeoInfoW(GEO_ISO2). Returns false when Windows
// has no two-letter code (a UN M.49 area such as "419", none set) or the call failed;
// `last_error`, when not null, then receives GetLastError() (0 for a non-ISO answer).
bool HomeRegion(char16_t out[3], unsigned long* last_error);

}  // namespace dev
