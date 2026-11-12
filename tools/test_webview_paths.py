#!/usr/bin/env python3
"""Run the production upload path JNI function with real Java namespace mapping.

Only the executable-path query is adapted to a VM on D:. The old Win32 full-path
query has an explicit D: host envelope for the optional old-source control.
Browser file selection, upload bytes and network behavior are not exercised.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

ENVELOPE = r'''
#include <jni.h>
#include <string>
#include <cwchar>
[[maybe_unused]] static std::wstring Str(JNIEnv* e, jstring s) {
  if (!s) return {};
  const jchar* p=e->GetStringChars(s,nullptr);
  std::wstring result(p,p+e->GetStringLength(s));
  e->ReleaseStringChars(s,p); return result;
}
[[maybe_unused]] static jstring JStr(JNIEnv* e,const std::wstring& s) {
  std::u16string value(s.begin(),s.end());
  return e->NewString(reinterpret_cast<const jchar*>(value.data()),value.size());
}
using DWORD=unsigned long;
constexpr int MAX_PATH=260;
[[maybe_unused]] static DWORD GetFullPathNameW(const wchar_t* path,DWORD count,wchar_t* out,void*) {
  std::wstring p(path);
  if (!p.empty() && p[0]==L'\\') p=L"D:"+p;
  else if (p.size()<2 || p[1]!=L':') p=L"D:\\current\\"+p;
  if (p.size()+1<=count) std::wmemcpy(out,p.c_str(),p.size()+1);
  return p.size();
}
@FUNCTION@
extern "C" JNIEXPORT jstring JNICALL Java_WebViewPathHost_convert(JNIEnv* e,jclass c,jstring p) {
  return WebView2Host_windowsPath(e,c,p);
}
extern "C" JNIEXPORT jstring JNICALL Java_com_gesher_server_Zygote_nativeExePath(JNIEnv* e,jclass) {
  return e->NewStringUTF("D:\\gesher\\dalvikvm.exe");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java-home", required=True, type=Path)
    parser.add_argument("--framework-classes", required=True, type=Path)
    parser.add_argument("--core-classes", required=True, type=Path)
    parser.add_argument("--app-classes", required=True, type=Path)
    parser.add_argument("--source-ref")
    args = parser.parse_args()
    relative = "compat/webview/webview_jni.cc"
    source = (ROOT / relative).read_text()
    if args.source_ref:
        source = subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                cwd=ROOT, capture_output=True, text=True, check=True).stdout
    start = "jstring JNICALL WebView2Host_windowsPath("
    end = "void JNICALL WebView2Host_navigateToString("
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError("Upload path JNI source boundaries changed")
    function = source[source.index(start):source.index(end)]
    classpath = os.pathsep.join(str(path.resolve()) for path in
                               (args.app_classes, args.framework_classes, args.core_classes))
    java_home = args.java_home.resolve()
    with tempfile.TemporaryDirectory(prefix="gesher-webview-paths-") as temporary:
        work = Path(temporary)
        cpp, library = work / "paths.cc", work / "libpaths.so"
        cpp.write_text(ENVELOPE.replace("@FUNCTION@", function))
        subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                        "-I" + str(java_home / "include"), "-I" + str(java_home / "include/linux"),
                        str(cpp), "-o", str(library)], check=True, timeout=90)
        subprocess.run([str(java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(work), str(ROOT / "app/java/com/gesher/server/Zygote.java"),
                        str(ROOT / "tests/webview-paths/WebViewPathHost.java"),
                        str(ROOT / "tests/dex-services/gesher/services/WebViewUploadPathsTest.java")],
                       check=True, timeout=60)
        subprocess.run([str(java_home / "bin/java"), "-Xcheck:jni", "-XX:-UsePerfData", "-cp",
                        str(work) + os.pathsep + classpath, "WebViewPathHost", str(library)],
                       check=True, timeout=30)


if __name__ == "__main__":
    main()
