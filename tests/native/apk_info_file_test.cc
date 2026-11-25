// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Run on Windows: the production parser of `gesher-app apk-info`'s output, as
// gesher-launch's install confirmation reads it (real UTF-8 conversion). It
// checks parsing and the same-version decision, not the dialogs.
#include "../../compat/shell/apk_info_file.h"

#include <cstdio>

static int checks;
static int failures;

static void Check(const char* label, bool ok) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", label);
    ++failures;
  }
}

int main() {
  GesherApkInfo info;
  Check("minimal", GesherParseApkInfo("package=com.example.app\nversion_code=42\n", &info) &&
                       info.package == L"com.example.app" && info.version_code == 42 &&
                       info.label == L"com.example.app" && !info.installed &&
                       !info.desktop_shortcut && !GesherApkInfoSameVersion(info));

  Check("all keys, CRLF, BOM, UTF-8",
        GesherParseApkInfo("\xEF\xBB\xBFpackage=org.sample\r\nversion_code=7\r\n"
                           "version_name=1.2 beta\r\nlabel=\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D\r\n"
                           "icon=C:\\Users\\a b\\icons\\x=1.ico\r\n"
                           "installed_version_code=6\r\ninstalled_version_name=1.1\r\n"
                           "desktop_shortcut=1\r\n",
                           &info) &&
            info.package == L"org.sample" && info.version_code == 7 &&
            info.version_name == L"1.2 beta" && info.label == L"\u05e9\u05dc\u05d5\u05dd" &&
            info.icon == L"C:\\Users\\a b\\icons\\x=1.ico" && info.installed &&
            info.installed_version_valid && info.installed_version_code == 6 &&
            info.installed_version_name == L"1.1" && info.desktop_shortcut &&
            !GesherApkInfoSameVersion(info));

  Check("same version",
        GesherParseApkInfo("package=a.b\nversion_code=9\ninstalled_version_code=9\n", &info) &&
            GesherApkInfoSameVersion(info));
  Check("bad installed version is not the same version",
        GesherParseApkInfo("package=a.b\nversion_code=9\ninstalled_version_code=9x\n", &info) &&
            info.installed && !info.installed_version_valid && !GesherApkInfoSameVersion(info));
  Check("long version codes",
        GesherParseApkInfo("package=a.b\nversion_code=9223372036854775807\n"
                           "installed_version_code=-9223372036854775808\n",
                           &info) &&
            info.version_code == 9223372036854775807LL &&
            info.installed_version_code == -9223372036854775807LL - 1);
  Check("version code overflow",
        !GesherParseApkInfo("package=a.b\nversion_code=9223372036854775808\n", &info));
  Check("empty version code", !GesherParseApkInfo("package=a.b\nversion_code=\n", &info));
  Check("spaced version code", !GesherParseApkInfo("package=a.b\nversion_code= 1\n", &info));
  Check("no version code", !GesherParseApkInfo("package=a.b\n", &info));
  Check("no package", !GesherParseApkInfo("version_code=1\n", &info));
  Check("empty file", !GesherParseApkInfo("", &info));
  Check("package with a quote",
        !GesherParseApkInfo("package=a.b\" --x\nversion_code=1\n", &info));
  Check("package with spaces", !GesherParseApkInfo("package=a b\nversion_code=1\n", &info));
  Check("package with empty segment", !GesherParseApkInfo("package=a..b\nversion_code=1\n", &info));
  Check("package starting with a dot", !GesherParseApkInfo("package=.a\nversion_code=1\n", &info));
  Check("unknown keys, comments and junk lines ignored",
        GesherParseApkInfo("# comment\n\nfuture=x\nnot a pair\n=x\npackage=a_b.c1\n"
                           "version_code=3\nlabel=  \tMy\tApp \n",
                           &info) &&
            info.package == L"a_b.c1" && info.label == L"My App");
  Check("last line wins",
        GesherParseApkInfo("package=a.b\nversion_code=1\nversion_code=2\n", &info) &&
            info.version_code == 2);
  Check("no final newline",
        GesherParseApkInfo("package=a.b\nversion_code=5", &info) && info.version_code == 5);
  Check("blank label falls back to the package",
        GesherParseApkInfo("package=a.b\nversion_code=5\nlabel= \n", &info) &&
            info.label == L"a.b");
  Check("desktop_shortcut only for 1",
        GesherParseApkInfo("package=a.b\nversion_code=5\ndesktop_shortcut=true\n", &info) &&
            !info.desktop_shortcut);
  Check("invalid UTF-8 does not fail the file",
        GesherParseApkInfo("package=a.b\nversion_code=5\nlabel=\xFF\xFE ok\n", &info) &&
            !info.label.empty());
  Check("value keeps '='",
        GesherParseApkInfo("package=a.b\nversion_code=5\nversion_name=a=b\n", &info) &&
            info.version_name == L"a=b");

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
