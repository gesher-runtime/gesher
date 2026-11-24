#!/usr/bin/env python3
"""Real AudioService/PermissionPolicy and production JNI microphone admission contracts.

JNI lookup-failure controls use the exact native resolver body with an explicit JNI boundary.
The JVM fixture crosses that same body into the actual production Java UID permission store.
Windows public adapters and native input capture remain separate required integration gates.
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def source(path: str, ref: str | None) -> str:
    if ref:
        return subprocess.run(["git", "show", ref + ":" + path], cwd=ROOT,
                              check=True, capture_output=True, text=True).stdout
    return (ROOT / path).read_text()


def method(text: str, signature: str) -> str:
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        if text[end] == "{": depth += 1
        elif text[end] == "}": depth -= 1
        end += 1
    return text[start:end]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java-home", type=Path, required=True)
    parser.add_argument("--framework-classes", type=Path)
    parser.add_argument("--core-classes", type=Path)
    parser.add_argument("--app-classes", type=Path)
    parser.add_argument("--apksig", type=Path)
    parser.add_argument("--source-ref", help="Actual older production Java/native bodies for negative controls")
    parser.add_argument("--native-only", action="store_true")
    parser.add_argument("--java-only", action="store_true")
    args = parser.parse_args()
    if args.native_only and args.java_only: parser.error("Choose at most one limited scenario")
    if not args.native_only and not all((args.framework_classes, args.core_classes, args.app_classes, args.apksig)):
        parser.error("JVM fixture needs framework/core/app classes and apksig")
    cc, cxx = shutil.which("cc"), shutil.which("c++")
    if not cc or not cxx: parser.error("C and C++ compilers are required for the JNI boundaries")
    native = method(source("compat/media/jni/audio_record_jni.cc", args.source_ref), "bool MayRecordAudio(JNIEnv* env)")
    include = ["-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
               "-I" + str(ROOT / "compat/media/audio")]
    permission = str(ROOT / "compat/media/audio/record_permission.cc")
    with tempfile.TemporaryDirectory(prefix="gesher-audio-admission-") as temporary:
        work = Path(temporary)
        if not args.java_only:
            native_fixture = work / "native.cc"
            native_fixture.write_text((ROOT / "tests/audio/recording_permission_native.cc.in").read_text()
                                      .replace("@MAY_RECORD_AUDIO@", native))
            executable = work / "native"
            subprocess.run([cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *include,
                            str(native_fixture), permission, "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=30)
        if args.native_only: return
        bridge = work / "bridge.cc"
        bridge.write_text("""#include <jni.h>
#include "record_permission.h"
using namespace gesher::media;
static JavaVM* vm;
static JNIEnv* AttachedEnv(const char*) {
    JNIEnv* env = nullptr;
    return vm && vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK ? env : nullptr;
}
static bool CheckException(JNIEnv* env, const char*) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear(); return true;
}
""" + native + """
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* runtime, void*) { vm = runtime; return JNI_VERSION_1_6; }
extern "C" JNIEXPORT jboolean JNICALL
Java_gesher_services_AudioRecordingPermissionTest_nativeAllowed(JNIEnv* env, jclass, jboolean provider) {
    if (!provider) return MayRecordAudio(env) ? JNI_TRUE : JNI_FALSE;
    SetRecordingPermissionProvider([] { return MayRecordAudio(nullptr); });
    return RecordingAllowed() ? JNI_TRUE : JNI_FALSE;
}
""")
        boundaries = work / "boundaries.o"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-fPIC", *include,
                        "-c", str(ROOT / "tests/activity-finish/framework_host.c"), "-o", str(boundaries)], check=True)
        library = work / "libaudio_admission.so"
        subprocess.run([cxx, "-std=c++17", "-shared", "-fPIC", *include,
                        str(bridge), permission, str(boundaries), "-o", str(library)], check=True)
        classes = work / "classes"; classes.mkdir()
        service = work / "AudioService.java"
        service.write_text(source("app/java/com/gesher/server/AudioService.java", args.source_ref))
        classpath = os.pathsep.join(str(path.resolve()) for path in
                                   (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
        subprocess.run([str(args.java_home / "bin/javac"), "-J-XX:-UsePerfData", "-encoding", "UTF-8",
                        "-cp", classpath, "-d", str(classes), str(service),
                        str(ROOT / "tests/dex-services/gesher/services/AudioRecordingPermissionTest.java"),
                        str(ROOT / "tests/audio/AudioRecordingPermissionHost.java")], check=True)
        cleaner = work / "cleaner/sun/misc"; cleaner.mkdir(parents=True)
        for entry in (args.core_classes / "sun/misc").glob("Cleaner*.class"):
            shutil.copy2(entry, cleaner / entry.name)
        environment = os.environ.copy()
        environment["ANDROID_DATA"] = str(work / "data")
        environment["GESHER_MICROG_SPOOF"] = "1"
        environment.pop("GESHER_BINDER_UID", None)
        subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData", "-Djava.io.tmpdir=" + str(work),
                        "--patch-module", "jdk.unsupported=" + str(work / "cleaner"),
                        "-cp", str(classes) + os.pathsep + classpath,
                        "AudioRecordingPermissionHost", str(library)], check=True, env=environment, timeout=90)


if __name__ == "__main__":
    main()
