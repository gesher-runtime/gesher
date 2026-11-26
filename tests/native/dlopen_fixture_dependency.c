// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Isolated DLL fixture: the production loader must find this sibling import.
__declspec(dllexport) int gesher_dlopen_dependency(void) { return 73; }
