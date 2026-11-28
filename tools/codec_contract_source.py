"""Read exact codec contract bodies across the JNI-to-engine extraction.

Portable fixtures supply object fields and external boundaries. Moving an
out-of-line engine definition into that fixture changes only its class qualifier
and declaration defaults; its executable body remains byte-for-byte unchanged.
"""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
JNI = "compat/media/jni/media_codec_jni.cc"
ENGINE = "compat/media/codec/codec_engine.cc"
HEADER = "compat/media/codec/codec_engine.h"


def read(path: str, ref: str | None = None) -> str:
    if ref:
        return subprocess.check_output(["git", "show", ref + ":" + path], cwd=ROOT, text=True)
    return (ROOT / path).read_text()


def shared(ref: str | None = None) -> bool:
    return '#include "../codec/codec_engine.h"' in read(JNI, ref)


def method(source: str, marker: str) -> str:
    if source.count(marker) != 1:
        raise ValueError("production boundary changed: " + marker)
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth and end < len(source):
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    if depth:
        raise ValueError("unbalanced production method: " + marker)
    return source[start:end]


def member(source: str, name: str) -> str:
    matches = list(re.finditer(r"^([^\n]*?)CodecEngine::" + re.escape(name) + r"\(", source, re.M))
    if len(matches) != 1:
        raise ValueError("engine method boundary changed: " + name)
    result = method(source, matches[0].group())
    declaration, body = result.split("{", 1)
    declaration = declaration.replace("CodecEngine::", "", 1).replace("~CodecEngine", "~Codec")
    # Defaults are declared in codec_engine.h, not in the out-of-line definition.
    declaration = re.sub(r"int\* (status|release_status|out_data_size)(?=\s*[,)]|\s*$)",
                         r"int* \1 = nullptr", declaration)
    return declaration + "{" + body


def adapter(source: str, marker: str) -> str:
    result = method(source, marker)
    declaration, body = result.split("{", 1)
    declaration = re.sub(r"int\* status(?=\s*[,)]|\s*$)", "int* status = nullptr", declaration)
    return declaration + "{" + body
