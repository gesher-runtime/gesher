/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Actual native controls on a private desktop, never shown to the user. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include "../../compat/locale/ui_locale.h"
#include "../../compat/windows/host_dialog_theme.h"
#include "../../compat/credentials/device_admin_dialog.inc"
static int choice, checks, failures;
static BOOL supported;
static enum GesherUiLanguage language;
static void Check(BOOL value, const char* why) { ++checks; if (!value) { ++failures; fprintf(stderr,"FAIL: %s\n",why); } }
static BOOL HasHebrew(const WCHAR* text) {
  for (; *text; ++text) if (*text >= 0x05d0 && *text <= 0x05ea) return TRUE;
  return FALSE;
}
static void CALLBACK Inspect(HWND dialog, UINT message, UINT_PTR timer, DWORD time) {
  (void)message; (void)time; KillTimer(dialog,timer);
  WCHAR text[32768];
  GetWindowTextW(GetDlgItem(dialog,GESHER_HOST_MESSAGE_TEXT),text,32768);
  Check(wcsstr(text,L"fixture.pkg/AdminReceiver") != NULL,"administrator identity visible");
  Check(wcsstr(text,GesherUiText(language,supported ? "device_admin_lock" : "device_admin_unsupported")) != NULL,"trusted localized policy explanation visible");
  Check(wcschr(text,0x2068) == NULL && wcschr(text,0x2069) == NULL,"no unsupported bidi isolates");
  if (language == GESHER_UI_HE) {
    Check(HasHebrew(text),"Hebrew policy contains actual Hebrew letters, not replacement question marks");
    WCHAR title[256], button[256]; GetWindowTextW(dialog,title,256);
    GetWindowTextW(GetDlgItem(dialog,supported ? IDYES : IDOK),button,256);
    Check(HasHebrew(title) && HasHebrew(button),"Hebrew title and action are translated");
  }
  if (supported) Check(wcsstr(text,GesherUiText(language,"device_admin_app_explanation")) != NULL
      && wcsstr(text,L"Fixture description") != NULL,"app information is labelled separately from trusted policy");
  if (supported) {
    Check(GetFocus() == GetDlgItem(dialog,IDCANCEL),"default focus denies enrollment");
    Check(LOWORD(SendMessageW(dialog,DM_GETDEFID,0,0)) == IDCANCEL,"default button denies enrollment");
  }
  if (choice == 0) SendMessageW(GetDlgItem(dialog,supported ? IDYES : IDOK),BM_CLICK,0,0);
  else if (choice == 1) SendMessageW(GetDlgItem(dialog,IDCANCEL),BM_CLICK,0,0);
  else if (choice == 2) PostMessageW(dialog,WM_CLOSE,0,0);
  else PostMessageW(GetFocus(),WM_KEYDOWN,choice == 3 ? VK_RETURN : VK_ESCAPE,0);
}
static LRESULT CALLBACK Hook(int code, WPARAM wp, LPARAM lp) {
  if (code >= 0) { CWPRETSTRUCT* event=(CWPRETSTRUCT*)lp;
    if (event->message == WM_INITDIALOG) SetTimer(event->hwnd,1,20,Inspect);
  }
  return CallNextHookEx(NULL,code,wp,lp);
}
static DWORD WINAPI Watchdog(LPVOID complete) { if (WaitForSingleObject(complete,60000) == WAIT_TIMEOUT) ExitProcess(99); return 0; }
int main(void) {
  HANDLE completed=CreateEventW(NULL,TRUE,FALSE,NULL);
  CreateThread(NULL,0,Watchdog,completed,0,NULL);
  WCHAR name[80]; swprintf(name,80,L"gesher-admin-ui-%lu",GetCurrentProcessId());
  HDESK original=GetThreadDesktop(GetCurrentThreadId());
  HDESK desktop=CreateDesktopW(name,NULL,NULL,0,GENERIC_ALL,NULL);
  if (!desktop || !SetThreadDesktop(desktop)) return 2;
  HHOOK hook=SetWindowsHookExW(WH_CALLWNDPROCRET,Hook,NULL,GetCurrentThreadId());
  if (!hook) return 3;
  for (int locale=0;locale<2;++locale) {
    language=locale ? GESHER_UI_HE : GESHER_UI_EN;
    supported=TRUE;
    for (choice=0;choice<5;++choice) {
      BOOL accepted=DeviceAdminDialog(language,L"FaceLock\nfixture.pkg/AdminReceiver",L"Fixture description",TRUE);
      Check(accepted == (choice == 0),"only explicit Activate accepts enrollment");
    }
    supported=FALSE; choice=0;
    Check(!DeviceAdminDialog(language,L"fixture.pkg/AdminReceiver",L"",FALSE),"unsupported policy acknowledgement cannot activate");
  }
  UnhookWindowsHookEx(hook); SetThreadDesktop(original); CloseDesktop(desktop); SetEvent(completed);
  printf("device admin native UI: %d checks, %d failures\n",checks,failures);
  return failures ? 1 : 0;
}
