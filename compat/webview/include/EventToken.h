// Gesher: WebView2.h includes "EventToken.h" (the Windows SDK's spelling);
// mingw-w64 names it eventtoken.h, and the cross build runs on a
// case-sensitive file system (docs/webview.md).
#pragma once

#if __has_include(<eventtoken.h>)
#include <eventtoken.h>
#else
typedef struct EventRegistrationToken {
  __int64 value;
} EventRegistrationToken;
#endif
