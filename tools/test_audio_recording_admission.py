#!/usr/bin/env python3
"""Real AudioService/PermissionPolicy and production JNI microphone admission contracts.

JNI lookup-failure controls use the exact native resolver body with an explicit JNI boundary.
The JVM fixture crosses that same body into the actual production Java UID permission store
from both attached Java threads and detached native threads (AAudio / OpenSL ES).
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

BRIDGE_PREAMBLE = """#include <jni.h>
#include <atomic>
#include <thread>
#include "record_permission.h"
using namespace gesher::media;
static std::atomic<JavaVM*> g_jvm{nullptr};
[[maybe_unused]] static JavaVM* Vm() { return g_jvm.load(std::memory_order_acquire); }
[[maybe_unused]] static JNIEnv* AttachedEnv(const char*) {
    JavaVM* vm = g_jvm.load(std::memory_order_acquire);
    void* env = nullptr;
    return vm && vm->GetEnv(&env, JNI_VERSION_1_6) == JNI_OK ? static_cast<JNIEnv*>(env) : nullptr;
}
static bool CheckException(JNIEnv* env, const char*) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear(); return true;
}
"""

BRIDGE_EXPORTS = """
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* runtime, void*) {
    g_jvm.store(runtime, std::memory_order_release);
    SetRecordingPermissionProvider([] { return MayRecordAudio(nullptr); });
    return JNI_VERSION_1_6;
}
static jboolean DetachedAllowedCheck(JNIEnv* caller_env) {
    JavaVM* vm = g_jvm.load(std::memory_order_acquire);
    bool allowed = false;
    bool detached_before = false;
    bool detached_after = false;
    std::thread worker([&] {
        void* probe = nullptr;
        detached_before = vm != nullptr && vm->GetEnv(&probe, JNI_VERSION_1_6) == JNI_EDETACHED && probe == nullptr;
        allowed = RecordingAllowed();
        probe = reinterpret_cast<void*>(1);
        detached_after = vm != nullptr && vm->GetEnv(&probe, JNI_VERSION_1_6) == JNI_EDETACHED && probe == nullptr;
    });
    worker.join();
    if (!detached_before || !detached_after) {
        jclass err = caller_env->FindClass("java/lang/AssertionError");
        if (err != nullptr) {
            caller_env->ThrowNew(err, "Detached native thread must start detached and RAII-detach after MayRecordAudio");
        }
        return JNI_FALSE;
    }
    return allowed ? JNI_TRUE : JNI_FALSE;
}
extern "C" JNIEXPORT jboolean JNICALL
Java_gesher_services_AudioRecordingPermissionTest_nativeAllowed(JNIEnv* env, jclass, jboolean provider) {
    if (!provider) return MayRecordAudio(env) ? JNI_TRUE : JNI_FALSE;
    SetRecordingPermissionProvider([] { return MayRecordAudio(nullptr); });
    return RecordingAllowed() ? JNI_TRUE : JNI_FALSE;
}
extern "C" JNIEXPORT jboolean JNICALL
Java_gesher_services_AudioRecordingPermissionTest_nativeDetachedAllowed(JNIEnv* env, jclass) {
    SetRecordingPermissionProvider([] { return MayRecordAudio(nullptr); });
    return DetachedAllowedCheck(env);
}
extern "C" JNIEXPORT jboolean JNICALL
Java_NativeDetachedJvmHost_nativeAttached(JNIEnv* env, jclass, jboolean viaProvider) {
    if (!viaProvider) return MayRecordAudio(env) ? JNI_TRUE : JNI_FALSE;
    return RecordingAllowed() ? JNI_TRUE : JNI_FALSE;
}
extern "C" JNIEXPORT jboolean JNICALL
Java_NativeDetachedJvmHost_nativeDetached(JNIEnv* env, jclass) {
    return DetachedAllowedCheck(env);
}
extern "C" JNIEXPORT jboolean JNICALL
Java_NativeDetachedJvmHost_nativeWithoutVm(JNIEnv*, jclass) {
    JavaVM* saved = g_jvm.exchange(nullptr, std::memory_order_acq_rel);
    bool allowed = false;
    std::thread worker([&] { allowed = RecordingAllowed(); });
    worker.join();
    g_jvm.store(saved, std::memory_order_release);
    return allowed ? JNI_TRUE : JNI_FALSE;
}
"""


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


def run_host_jvm_jni_gate(cxx: str, java_home: Path, include: list[str],
                          permission: str, bridge: Path, work: Path) -> None:
    jvm_dir = work / "jvm_host"
    (jvm_dir / "com/gesher/server").mkdir(parents=True)
    (jvm_dir / "com/gesher/server/AudioService.java").write_text(
        """package com.gesher.server;
public final class AudioService {
    public static volatile boolean granted;
    public static volatile boolean fail;
    public static boolean recordAudioAllowed() {
        if (fail) throw new IllegalStateException("simulated policy failure");
        return granted;
    }
}
""", encoding="utf-8")
    (jvm_dir / "NativeDetachedJvmHost.java").write_text(
        """import com.gesher.server.AudioService;
public final class NativeDetachedJvmHost {
    private static int checks;
    private static native boolean nativeAttached(boolean viaProvider);
    private static native boolean nativeDetached();
    private static native boolean nativeWithoutVm();
    private static void check(boolean ok, String msg) {
        if (!ok) throw new AssertionError(msg);
        checks++;
    }
    public static void main(String[] args) {
        System.load(args[0]);
        AudioService.granted = false;
        AudioService.fail = false;
        check(!nativeAttached(false), "attached MayRecordAudio(env) denies when revoked");
        check(!nativeAttached(true), "attached MayRecordAudio(nullptr) denies when revoked");
        check(!nativeDetached(), "detached std::thread MayRecordAudio(nullptr) denies when revoked");

        AudioService.granted = true;
        check(nativeAttached(false), "attached MayRecordAudio(env) allows when granted");
        check(nativeAttached(true), "attached MayRecordAudio(nullptr) allows when granted");
        check(nativeDetached(), "detached std::thread MayRecordAudio(nullptr) allows when granted");
        check(!nativeWithoutVm(), "detached thread fails closed when JavaVM* is absent");

        AudioService.fail = true;
        check(!nativeAttached(true), "attached thread fails closed when Java policy throws");
        check(!nativeDetached(), "detached thread clears exception and RAII-detaches when Java policy throws");

        AudioService.fail = false;
        AudioService.granted = false;
        check(!nativeDetached(), "detached thread observes revocation immediately after grant");
        AudioService.granted = true;
        check(nativeDetached(), "detached thread observes re-grant after exception and revocation");
        System.out.println("Host JVM JNI attached/detached audio admission passed: " + checks + " checks");
    }
}
""", encoding="utf-8")
    jvm_lib = jvm_dir / "libaudio_admission_jvm.so"
    subprocess.run([cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-pthread",
                    "-fsanitize=undefined", "-fno-sanitize-recover=undefined",
                    *include, str(bridge), permission, "-o", str(jvm_lib)], check=True)
    subprocess.run([str(java_home / "bin/javac"), "-J-XX:-UsePerfData", "-encoding", "UTF-8",
                    "-d", str(jvm_dir),
                    str(jvm_dir / "com/gesher/server/AudioService.java"),
                    str(jvm_dir / "NativeDetachedJvmHost.java")], check=True)
    subprocess.run([str(java_home / "bin/java"), "-XX:-UsePerfData", "-Xcheck:jni",
                    "-cp", str(jvm_dir), "NativeDetachedJvmHost", str(jvm_lib)],
                   check=True, timeout=60)


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
    parser.add_argument("--sanitize", action="store_true", help="Build standalone C++ resolver test with ASan/UBSan")
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
        bridge = work / "bridge.cc"
        bridge.write_text(BRIDGE_PREAMBLE + native + BRIDGE_EXPORTS, encoding="utf-8")
        if not args.java_only:
            native_fixture = work / "native.cc"
            native_fixture.write_text((ROOT / "tests/audio/recording_permission_native.cc.in").read_text()
                                      .replace("@MAY_RECORD_AUDIO@", native))
            executable = work / "native"
            sanitize_flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                              "-fno-omit-frame-pointer"] if args.sanitize else []
            subprocess.run([cxx, "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            *sanitize_flags, *include,
                            str(native_fixture), permission, "-o", str(executable)], check=True)
            env = os.environ.copy()
            if args.sanitize:
                env["UBSAN_OPTIONS"] = "halt_on_error=1:" + env.get("UBSAN_OPTIONS", "")
            subprocess.run([str(executable)], check=True, env=env, timeout=30)
            run_host_jvm_jni_gate(cxx, args.java_home, include, permission, bridge, work)
        if args.native_only: return
        boundaries = work / "boundaries.o"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-fPIC", *include,
                        "-c", str(ROOT / "tests/activity-finish/framework_host.c"), "-o", str(boundaries)], check=True)
        library = work / "libaudio_admission.so"
        subprocess.run([cxx, "-std=c++17", "-shared", "-fPIC", "-pthread", *include,
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
