#!/usr/bin/env python3
"""Check production recorder state and JNI errors against Android 16 contracts.

Compiles the complete Recorder and writer translation units and the production
JNI namespace on a host JVM. The existing recorder-host capture/encoder/Surface
fixtures are the only backend substitutions. They are test-only and do not
validate Windows devices, codecs, Android permissions, ART, or boot registration.
FileDescriptor.descriptor is adapted to the host JDK's fd field. JNI types come
from the real JDK, not the minimal native-only lifecycle fixture header.
"""
from __future__ import annotations
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

REGISTRATION = r'''
namespace gesher::media {
void HostControls(bool, bool);
void HostFeed(unsigned);
bool HostCaptureReleased();
int HostEncodedParameter(int);
void JNICALL Controls(JNIEnv*, jclass, jboolean denied, jboolean failure) { HostControls(denied, failure); }
void JNICALL Feed(JNIEnv*, jclass, jint batches) { HostFeed(static_cast<unsigned>(batches)); }
jboolean JNICALL Released(JNIEnv*, jclass) { return HostCaptureReleased(); }
jint JNICALL Parameter(JNIEnv*, jclass, jint which) { return HostEncodedParameter(which); }
}
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  using namespace gesher::media;
  host_vm = vm;
  HostControls(false, false);
  JNIEnv* env = AttachedEnv("host");
  jclass cls = env->FindClass("MediaRecorderStateHost");
  if (cls == nullptr) return JNI_ERR;
  gF.context = env->GetFieldID(cls, "context", "J");
  gF.clazz = static_cast<jclass>(env->NewGlobalRef(cls));
  gF.post_event = env->GetStaticMethodID(cls, "event", "(Ljava/lang/Object;IIILjava/lang/Object;)V");
  const JNINativeMethod methods[] = {
    {"setup", "(Ljava/lang/Object;Ljava/lang/String;Ljava/lang/Object;)V", GESHER_NATIVE(MR_setup)},
    {"release", "()V", GESHER_NATIVE(MR_release)}, {"reset", "()V", GESHER_NATIVE(MR_reset)},
    {"source", "(I)V", GESHER_NATIVE(MR_setAudioSource)},
    {"videoSource", "(I)V", GESHER_NATIVE(MR_setVideoSource)},
    {"format", "(I)V", GESHER_NATIVE(MR_setOutputFormat)},
    {"audioEncoder", "(I)V", GESHER_NATIVE(MR_setAudioEncoder)},
    {"videoEncoder", "(I)V", GESHER_NATIVE(MR_setVideoEncoder)},
    {"videoRate", "(I)V", GESHER_NATIVE(MR_setVideoFrameRate)},
    {"duration", "(I)V", GESHER_NATIVE(MR_setMaxDuration)},
    {"size", "(J)V", GESHER_NATIVE(MR_setMaxFileSize)},
    {"parameter", "(Ljava/lang/String;)V", GESHER_NATIVE(MR_setParameter)},
    {"file", "(Ljava/io/FileDescriptor;)V", GESHER_NATIVE(MR_setOutputFile)},
    {"nextFile", "(Ljava/io/FileDescriptor;)V", GESHER_NATIVE(MR_setNextOutputFile)},
    {"prepare", "()V", GESHER_NATIVE(MR_prepare)}, {"start", "()V", GESHER_NATIVE(MR_start)},
    {"pause", "()V", GESHER_NATIVE(MR_pause)}, {"resume", "()V", GESHER_NATIVE(MR_resume)},
    {"stop", "()V", GESHER_NATIVE(MR_stop)}, {"amplitude", "()I", GESHER_NATIVE(MR_getMaxAmplitude)},
    {"camera", "(Ljava/lang/Object;)V", GESHER_NATIVE(MR_setCamera)},
    {"routes", "()[I", GESHER_NATIVE(MR_routedDevices)},
    {"controls", "(ZZ)V", GESHER_NATIVE(Controls)}, {"feed", "(I)V", GESHER_NATIVE(Feed)},
    {"captureReleased", "()Z", GESHER_NATIVE(Released)},
    {"encodedParameter", "(I)I", GESHER_NATIVE(Parameter)},
  };
  if (env->RegisterNatives(cls, methods, sizeof(methods) / sizeof(methods[0])) != JNI_OK) return JNI_ERR;
  env->DeleteLocalRef(cls);
  return JNI_VERSION_1_6;
}
'''
BACKEND_CONTROLS = r'''
static bool host_denied = false;
bool MayRecordAudio(JNIEnv*) { return !host_denied; }
void HostControls(bool denied, bool failure) {
  host_denied = denied; fail_capture_restart = failure;
  // Explicit non-decoding encoder fixture; independent container tests use
  // genuine encoded packets and validate their output with FFmpeg.
  access_units = {{0x01, 0x00}};
}
void HostFeed(unsigned batches) {
  const unsigned expected = encoded_batches + batches;
  feed_audio(batches);
  auto deadline = std::chrono::steady_clock::now() + 5s;
  while (encoded_batches < expected && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  assert(encoded_batches == expected);
}
bool HostCaptureReleased() { return capture_released; }
int HostEncodedParameter(int which) { return which == 0 ? observed_params.sample_rate : observed_params.bitrate; }
} // namespace gesher::media
'''


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, default=os.environ.get('JAVA_HOME'))
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    java = args.java_home or Path(shutil.which('javac') or 'missing').resolve().parent.parent
    host = (ROOT / 'tests/media/receiver_lifetime_host.h.in').read_text().split('struct Gate {')[0]
    global_refs = (ROOT / 'compat/media/jni/media_jni.h').read_text()
    global_refs = global_refs[global_refs.index('class GlobalRef {'):global_refs.index('// Throws a new')]
    host = host.replace('@GLOBAL_REFS@', global_refs)
    host = host.replace('#include <jni.h>', '#include <jni.h>\n#include "compat/media/record/recorder.h"')
    host = host.replace('inline bool MayRecordAudio(JNIEnv*) { return true; }', 'bool MayRecordAudio(JNIEnv*);')
    host += '}\n'
    backend = (ROOT / 'tests/media/recorder-host/lifecycle.cc').read_text().split('struct Events {')[0]
    # The shared encoder fixture records the received configuration directly;
    # preserve its actual bitrate getter and avoid rewriting factory signatures.
    source = (ROOT / 'compat/media/jni/media_recorder_jni.cc').read_text()
    print('media_recorder_jni SHA256 ' + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    body = source[source.index('namespace gesher::media {'):]
    assert body.count('"descriptor", "I"') == 1
    body = body.replace('"descriptor", "I"', '"fd", "I"')
    with tempfile.TemporaryDirectory(prefix='gesher-recorder-state-') as directory:
        work = Path(directory)
        (work / 'host.h').write_text(host)
        (work / 'jni.cc').write_text('#include "host.h"\n' + body + REGISTRATION)
        (work / 'backend.cc').write_text(backend + BACKEND_CONTROLS)
        media = ROOT / 'compat/media'
        library = work / 'librecorder_state.so'
        command = shlex.split(args.cxx) + ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-Wno-write-strings', '-Wno-unused-parameter', '-fno-exceptions', '-shared', '-fPIC', '-pthread',
            '-fsanitize=undefined', '-fno-sanitize-recover=undefined', '-DGESHER_MEDIA_STANDALONE',
            '-I', str(java / 'include'), '-I', str(java / 'include/linux'), '-I', str(ROOT),
            '-I', str(ROOT / 'tests/media/recorder-host/include'), str(work / 'jni.cc'), str(work / 'backend.cc'),
            str(media / 'record/recorder.cc'), str(media / 'mux/mp4_muxer.cc'), str(media / 'mux/ogg_muxer.cc'),
            str(media / 'encode/codec_config.cc'), '-o', str(library)]
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(java / 'bin/javac'), '-d', str(work),
                        str(ROOT / 'tests/media/MediaRecorderStateHost.java')], check=True, timeout=60)
        run = subprocess.run([str(java / 'bin/java'), '-Xcheck:jni', '-XX:-UsePerfData',
            '-XX:-CreateCoredumpOnCrash', '-cp', str(work), 'MediaRecorderStateHost', str(library), str(work)],
            capture_output=True, text=True, timeout=30)
        print(run.stdout, end=''); print(run.stderr, end='')
        return int(run.returncode != 0 or 'WARNING in native method' in run.stdout)

if __name__ == '__main__':
    raise SystemExit(main())
