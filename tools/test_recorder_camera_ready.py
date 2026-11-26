#!/usr/bin/env python3
"""Exercise the real recordapp completion callback and collection-start boundary.

The host uses opaque Camera2 identity objects and the actual JDK latch/atomics.
It does not run Media Foundation, synthesize a camera frame or verify video output;
the existing required Windows media shard still performs those operations.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests/apps/recordapp/java/com/gesher/recordapp/RecordActivity.java"


def components(source):
    camera = source.split("    void recorderCamera() throws Exception {", 1)[1]
    camera = camera.split("    // --- MediaCodec into an ImageReader", 1)[0]
    assert "boolean captureReady = " in camera, "owned completion must precede collection"
    readiness = re.search(r"boolean captureReady = (.*?);", camera, re.S).group(1)
    assert "firstCaptureCompleted.await(10, TimeUnit.SECONDS)" in readiness, "readiness wait remains bounded"
    assert camera.index("boolean captureReady = ") < camera.index("long collectStartNs = "), "owned completion must precede collection"
    assert "if (captureReady) Thread.sleep(2000);" in camera, "the original two-second collection requires readiness"
    assert "if (!captureReady) {\n            throw new IllegalStateException(" in camera, "readiness failure remains explicit"
    assert camera.index("r.release();", camera.index("boolean captureReady = ")) < camera.index("if (!captureReady)"), "readiness failure releases the recorder"
    assert camera.index("device[0].close();", camera.index("boolean captureReady = ")) < camera.index("if (!captureReady)"), "readiness failure closes the real camera"
    assert "opened.await(10, TimeUnit.SECONDS)" in camera and "configured.await(10, TimeUnit.SECONDS)" in camera
    assert "closed.await(5, TimeUnit.SECONDS)" in camera and "ht.join(2000)" in camera
    begin = camera.index("public void onCaptureCompleted(")
    end = camera.index("\n            @Override", begin)
    callback = camera[begin:end].rstrip()
    return callback, readiness


def run_variant(directory, source, javac, java, timeout=False):
    callback, readiness = components(source)
    template = (ROOT / "tests/media/recorder_camera_ready_host.java.in").read_text()
    generated = template.replace("@COMPLETION_METHOD@", callback).replace("@READINESS_EXPRESSION@", readiness)
    path = directory / "RecorderCameraReadyHost.java"
    path.write_text(generated)
    subprocess.run([javac, "-Xlint:all", "-Werror", "-d", str(directory), str(path)], check=True)
    return subprocess.run([java, "-cp", str(directory), "RecorderCameraReadyHost"] + (["timeout"] if timeout else []),
                          text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java-home", default=os.environ.get("JAVA_HOME"))
    parser.add_argument("--negative-controls", action="store_true")
    args = parser.parse_args()
    javac = str(Path(args.java_home) / "bin/javac") if args.java_home else "javac"
    java = str(Path(args.java_home) / "bin/java") if args.java_home else "java"
    source = SOURCE.read_text()
    with tempfile.TemporaryDirectory(prefix="gesher-recorder-ready-") as temp:
        output = Path(temp)
        candidate = run_variant(output, source, javac, java, timeout=True)
        print(candidate.stdout, end="")
        assert candidate.returncode == 0, "actual fixture callback/readiness failed"
        if args.negative_controls:
            mutants = [
                ("session", "s == recordingSession && ", "", "foreign session cannot begin collection"),
                ("callback-request", "request == recordingRequest", "true", "foreign callback request cannot begin collection"),
                ("result-request", "result.getRequest() == recordingRequest", "true", "foreign result request cannot begin collection"),
                ("sequence", "firstCompletedSequence.get() == requestId", "true", "sequence mismatch cannot begin collection"),
            ]
            for name, before, after, assertion in mutants:
                assert source.count(before) == 1, name
                result = run_variant(output, source.replace(before, after), javac, java)
                assert result.returncode != 0 and ("AssertionError: " + assertion) in result.stdout, (name, result.stdout)
                print("RECORDER_CAMERA_READY_NEGATIVE_PASS " + name)
    print("recordapp_sha256=" + hashlib.sha256(SOURCE.read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
