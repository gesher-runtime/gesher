// Gesher: WebView2.h includes "EventToken.h" (the Windows SDK's spelling);
// mingw-w64 names it eventtoken.h, and the cross build runs on a
// case-sensitive file system (docs/webview.md). include_next: on a
// case-insensitive one (a Windows host) <eventtoken.h> is this file.
#pragma once

#if __has_include_next(<eventtoken.h>)
#include_next <eventtoken.h>
#else
typedef struct EventRegistrationToken {
  __int64 value;
} EventRegistrationToken;
#endif
