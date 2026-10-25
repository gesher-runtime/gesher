/* Gesher: Windows toasts for the notification service (devsvc track), in C:
 * mingw-w64's WinRT headers declare their interfaces for C++ with templates
 * that clash (IReference<BYTE> and IReference<boolean>), and for C as plain
 * vtables, which only a C compiler takes. devsvc_win32.cc calls these on
 * threads in COM's multithreaded apartment. */
#pragma once

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shows the toast `xml` for `app_id`, tagged `tag` in `group` (replacing an
 * earlier one with both); S_OK or the failing HRESULT. */
long gesher_toast_show(const wchar_t* app_id, const wchar_t* tag, const wchar_t* group,
                       const wchar_t* xml, int suppress_popup, int high);
/* Removes one toast from the screen and the notification center. */
long gesher_toast_remove(const wchar_t* app_id, const wchar_t* tag, const wchar_t* group);
/* Removes every toast of `app_id`. */
long gesher_toast_clear(const wchar_t* app_id);

/* Called (on a WinRT thread) when a toast this process showed is clicked;
 * devsvc_win32.cc defines it. */
void gesher_toast_activated(const wchar_t* app_id, const wchar_t* tag, const wchar_t* group,
                            const wchar_t* arguments);

#ifdef __cplusplus
}
#endif
