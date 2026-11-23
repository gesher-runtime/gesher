#!/usr/bin/env python3
"""Production callback adapter and receiver JNI on a real host JVM.

The parser is a declared deterministic boundary, not an AOSP/Windows decoder.
MediaCallbackContractTest separately exercises real containers on ART/Windows.
--negative-control disables one production safeguard in a disposable copy;
it must make the unchanged assertions fail. No mutation is written to the repo.
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

def section(text, start, end):
    if text.count(start) != 1 or text.count(end) != 1:
        raise ValueError("production section boundary changed: " + start)
    begin = text.index(start)
    return text[begin:text.index(end, begin)]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java-home", type=Path, default=os.environ.get("JAVA_HOME"))
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--negative-control", choices=("stale-art", "unavailable-callback", "reentrant-admission", "album-art-key"))
    args = parser.parse_args()
    java = args.java_home or Path(shutil.which("javac") or "missing-javac").resolve().parent.parent
    retriever = (ROOT / "compat/media/jni/media_jni_others.cc").read_text()
    extractor = (ROOT / "compat/media/jni/media_extractor_jni.cc").read_text()
    sections = {
        "RETRIEVER": "\n".join((section(retriever, "jfieldID gRetrieverContext;", "void JNICALL MMR_setDataSourcePath("),
                                  section(retriever, "void JNICALL MMR_setDataSourceCallback(",
                                          "template <typename Consumer>\nvoid VisitRetrieverFrames("),
                                  section(retriever, "void JNICALL MMR_setup(", "const JNINativeMethod kRetrieverMethods[]"))),
        "EXTRACTOR": "\n".join((section(extractor, "jfieldID gContext;", "jobject Boxed("),
                                  section(extractor, "ReceiverLease<JniExtractor> GetForDataSource(", "void JNICALL ME_setDataSourcePath("),
                                  section(extractor, "void JNICALL ME_setDataSourceCallback(", "jobject JNICALL ME_getTrackFormat("),
                                  section(extractor, "void JNICALL ME_selectTrack(", "jboolean JNICALL ME_advance("),
                                  section(extractor, "jlong JNICALL ME_getSampleTime(", "jlong JNICALL ME_getSampleSize("),
                                  section(extractor, "void JNICALL ME_setup(", "jobject JNICALL ME_getAudioPresentations("))),
    }
    if args.negative_control == "stale-art":
        sections["RETRIEVER"] = sections["RETRIEVER"].replace("RetrieverSnapshot next;", "RetrieverSnapshot next = r->snapshot;")
    elif args.negative_control == "album-art-key":
        lookup = 'file.GetBuffer("albumart", &r->album_art);'
        if sections["RETRIEVER"].count(lookup) != 1:
            raise ValueError("production album-art lookup changed")
        sections["RETRIEVER"] = sections["RETRIEVER"].replace(lookup, lookup.replace('"albumart"', '"album-art"'))
    elif args.negative_control == "unavailable-callback":
        sections["EXTRACTOR"] = sections["EXTRACTOR"].replace('Open(env, thiz, std::move(source), "MediaDataSource");', '(void)thiz;')
    template = (ROOT / "tests/media/media_callback_host.cc.in").read_text()
    for name, code in sections.items():
        print(name + " production section SHA256 " + hashlib.sha256(code.encode()).hexdigest(), flush=True)
        template = template.replace("@" + name + "@", code)
    failed = False
    with tempfile.TemporaryDirectory(prefix="gesher-media-callback-") as tmp:
        folder = Path(tmp); cpp = folder / "callback.cc"; cpp.write_text(template)
        library = folder / "libmedia_callback.so"
        if args.negative_control == "reentrant-admission":
            header = folder / "compat/media/jni/receiver_lease.h"
            header.parent.mkdir(parents=True)
            text = (ROOT / "compat/media/jni/receiver_lease.h").read_text()
            header.write_text(text.replace("if (receiver_->operation_active)", "if (false)"))
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions", "-pthread",
                    "-shared", "-fPIC", "-fsanitize=undefined", "-fno-sanitize-recover=undefined",
                    "-I", str(folder), "-I", str(ROOT), "-I", str(java / "include"), "-I", str(java / "include/linux"), str(cpp), str(ROOT / "compat/media/jni/java_data_source.cc"), "-o", str(library)]
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(java / "bin/javac"), "-d", str(folder), str(ROOT / "tests/media/MediaCallbackHostTest.java")], check=True, timeout=60)
        cases = ("retriever",) if args.negative_control in ("stale-art", "album-art-key") else (("extractor",) if args.negative_control == "unavailable-callback" else (("reentrant",) if args.negative_control else ("adapter", "retriever", "extractor", "release", "reentrant")))
        for case in cases:
            run = subprocess.run([str(java / "bin/java"), "-Xcheck:jni", "-XX:-CreateCoredumpOnCrash", "-cp", str(folder),
                                  "MediaCallbackHostTest", str(library), case], capture_output=True, text=True, timeout=30)
            print(run.stdout, end=""); print(run.stderr, end="")
            failed |= run.returncode != 0 or "WARNING in native method" in run.stdout
    return int(failed)
if __name__ == "__main__": raise SystemExit(main())
