// Production namespace conversion only; actual SMB I/O is a Windows fixture.
#include <gesher/unc_paths.h>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
int checks;
void Check(bool value, const char* reason) {
  ++checks;
  if (!value) throw std::runtime_error(reason);
}
}

int main() {
  using gesher::unc_paths::Match;
  using gesher::unc_paths::FromWindows;
  using gesher::unc_paths::ToWindows;
  try {
    std::vector<std::pair<std::wstring, std::wstring>> paths = {
      {L"\\\\server\\share", L"/mnt/unc/server/share"},
      {L"\\\\server\\share\\", L"/mnt/unc/server/share/"},
      {L"\\\\SERVER\\Share Name\\space directory\\file.txt", L"/mnt/unc/SERVER/Share Name/space directory/file.txt"},
      {L"\\\\localhost\\C$\\Temp\\x", L"/mnt/unc/localhost/C$/Temp/x"},
      {L"\\\\שרת\\שיתוף\\תיקייה 😀\\file", L"/mnt/unc/שרת/שיתוף/תיקייה 😀/file"},
      {L"//server/share/path", L"/mnt/unc/server/share/path"},
      {L"\\\\?\\UNC\\server\\share\\path", L"/mnt/unc/server/share/path"},
      {L"\\\\?\\unc\\server\\share\\path", L"/mnt/unc/server/share/path"},
      {L"\\\\server\\share\\" + std::wstring(600, L'x'), L"/mnt/unc/server/share/" + std::wstring(600, L'x')}
    };
    for (const auto& [native, expected] : paths) {
      std::wstring guest, windows, roundtrip;
      Check(FromWindows(native, &guest), "Valid UNC return path rejected");
      Check(guest == expected, "UNC server/share/Unicode identity changed");
      Check(ToWindows(guest, &windows) == Match::kMapped, "Guest UNC path did not map");
      Check(windows.rfind(L"\\\\", 0) == 0 && windows.find(L'/') == std::wstring::npos,
            "Guest UNC path did not name a native share");
      Check(FromWindows(windows, &roundtrip) && roundtrip == expected,
            "Native/guest UNC round trip changed the file identity");
    }
    for (auto path : {L"", L"/", L"/mnt", L"/mnt/c/x", L"/mnt/uncx/file", L"/mnt/UNC/server/share",
                      L"relative/path", L"//server/share/path", L"\\\\server\\share\\path"}) {
      std::wstring output = L"untouched";
      Check(ToWindows(path, &output) == Match::kUnrelated && output == L"untouched",
            "UNC mapping captured an unrelated or raw native path");
    }
    for (auto path : {L"/mnt/unc", L"/mnt/unc/", L"/mnt/unc/server", L"/mnt/unc/server/",
                      L"/mnt/unc//share", L"/mnt/unc/./share", L"/mnt/unc/../share",
                      L"/mnt/unc/server/.", L"/mnt/unc/server/..", L"/mnt/unc/?/UNC/share",
                      L"/mnt/unc/server/share:stream", L"/mnt/unc/server\\share/path",
                      L"/mnt/unc:stream", L"/mnt/unc\\server\\share", L"/mnt/unc/ser\x7fver/share",
                      L"/mnt/unc/server/sh\tare", L"/mnt/unc/server/sh*are", L"/mnt/unc/server/sh?are"}) {
      std::wstring output = L"untouched";
      Check(ToWindows(path, &output) == Match::kInvalid && output == L"untouched",
            "Malformed UNC namespace was redirected to a native path");
    }
    for (auto path : {L"C:\\Temp", L"\\\\?\\C:\\Temp", L"\\rooted", L"\\\\server", L"\\\\server\\",
                      L"\\\\.\\pipe\\name", L"\\\\?\\GLOBALROOT\\Device\\Volume\\file",
                      L"\\\\?\\Volume{123}\\file", L"\\\\?\\UNC\\server", L"\\\\server\\..\\file"}) {
      std::wstring output = L"untouched";
      Check(!FromWindows(path, &output) && output == L"untouched",
            "Drive/device/malformed return path was treated as a UNC share");
    }
    std::printf("UNC namespace host checks: %d\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "UNC namespace failure after %d checks: %s\n", checks, error.what());
    return 1;
  }
}
