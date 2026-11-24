// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Whether Windows has a handler registered for a URI scheme (mailto, tel, sms, geo), as
// the shell would run it for ShellExecuteExW of such a URI. Used by
// com.gesher.server.WindowsUriHandlers through WindowsHost.hasUrlHandler.
#pragma once


namespace dev {

enum class UrlHandler {
  kRegistered,     // the current user's default resolves to a command or a DelegateExecute verb
  kNotRegistered,  // no association (or only the "Unknown" ProgID)
  kInvalidScheme,  // not an RFC 3986 scheme; nothing was asked of Windows
};

// AssocQueryStringW(ASSOCF_IS_PROTOCOL | ASSOCF_INIT_IGNOREUNKNOWN) for the "open" verb's
// ASSOCSTR_COMMAND, then ASSOCSTR_DELEGATEEXECUTE (packaged apps). `last_error`, when not
// null, receives the HRESULT (a long, so that this header needs no <windows.h>) of the last query (S_OK/S_FALSE when one was found).
UrlHandler QueryUrlHandler(const wchar_t* scheme, long* last_error);

}  // namespace dev
