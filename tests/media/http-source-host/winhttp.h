#pragma once
#include "windows.h"
constexpr int INTERNET_SCHEME_HTTP = 1, INTERNET_SCHEME_HTTPS = 2;
constexpr DWORD WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY = 4, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY = 0;
constexpr DWORD WINHTTP_FLAG_SECURE = 0x00800000;
constexpr DWORD WINHTTP_QUERY_STATUS_CODE = 19, WINHTTP_QUERY_FLAG_NUMBER = 0x20000000;
constexpr DWORD WINHTTP_QUERY_CONTENT_RANGE = 53, WINHTTP_QUERY_CONTENT_LENGTH = 5;
constexpr auto WINHTTP_NO_PROXY_NAME = nullptr;
constexpr auto WINHTTP_NO_PROXY_BYPASS = nullptr;
constexpr auto WINHTTP_NO_REFERER = nullptr;
constexpr auto WINHTTP_DEFAULT_ACCEPT_TYPES = nullptr;
constexpr auto WINHTTP_NO_ADDITIONAL_HEADERS = nullptr;
constexpr auto WINHTTP_NO_REQUEST_DATA = nullptr;
constexpr auto WINHTTP_HEADER_NAME_BY_INDEX = nullptr;
constexpr auto WINHTTP_NO_HEADER_INDEX = nullptr;
struct URL_COMPONENTS {
  DWORD dwStructSize = 0;
  wchar_t* lpszHostName = nullptr;
  DWORD dwHostNameLength = 0;
  wchar_t* lpszUrlPath = nullptr;
  DWORD dwUrlPathLength = 0;
  wchar_t* lpszExtraInfo = nullptr;
  DWORD dwExtraInfoLength = 0;
  int nScheme = INTERNET_SCHEME_HTTP;
  INTERNET_PORT nPort = 80;
};
BOOL WinHttpCrackUrl(const wchar_t*, DWORD, DWORD, URL_COMPONENTS*);
HINTERNET WinHttpOpen(const wchar_t*, DWORD, const wchar_t*, const wchar_t*, DWORD);
BOOL WinHttpCloseHandle(HINTERNET);
BOOL WinHttpSetTimeouts(HINTERNET, int, int, int, int);
HINTERNET WinHttpConnect(HINTERNET, const wchar_t*, INTERNET_PORT, DWORD);
HINTERNET WinHttpOpenRequest(HINTERNET, const wchar_t*, const wchar_t*, const wchar_t*,
                           const wchar_t*, const wchar_t* const*, DWORD);
BOOL WinHttpSendRequest(HINTERNET, const wchar_t*, DWORD, void*, DWORD, DWORD, uintptr_t);
BOOL WinHttpReceiveResponse(HINTERNET, void*);
BOOL WinHttpQueryHeaders(HINTERNET, DWORD, const wchar_t*, void*, DWORD*, DWORD*);
BOOL WinHttpQueryDataAvailable(HINTERNET, DWORD*);
BOOL WinHttpReadData(HINTERNET, void*, DWORD, DWORD*);
