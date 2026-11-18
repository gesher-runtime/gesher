#!/usr/bin/env python3
"""Run the production classloader adapter/catalog with real JVM weak references.

The complete production ELF tree is compiled unchanged, using the existing
explicit Linux Win32-primitive fixture. Fixture-owned providers and contextual
POSIX dispatch connect the real JVM to that loader; ART, Windows, NativeActivity
and JNI_OnLoad ownership are separate runtime gates. All DSOs are original valid
compiler outputs. The JVM fixture includes the whole loader translation unit to
inspect actual retained link state, without a production observer API. It calls
native-loader Reset after the real JVM is destroyed. The runner snapshots every
input and reports its digest.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import tempfile
import time

import test_elf_atomic_load as host


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jdk", type=Path, default=os.environ.get("JAVA_HOME"))
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--api-cache", type=Path, help="actual framework-api and core-api class directories")
    parser.add_argument("--generated-dependencies", type=Path, help="actual generated CompatChangeTable jar")
    args = parser.parse_args()
    if bool(args.api_cache) != bool(args.generated_dependencies):
        parser.error("Java policy execution needs both --api-cache and --generated-dependencies")
    if platform.system() != "Linux" or platform.machine().lower() not in ("amd64", "x86_64"):
        parser.error("the explicit JNI/Win32 host boundary requires Linux x86-64")
    jdk = args.jdk
    if jdk is None:
        javac = shutil.which("javac")
        if javac is None:
            parser.error("provide --jdk or JAVA_HOME for a real JVM and JNI headers")
        jdk = Path(javac).resolve().parent.parent
    jdk = Path(jdk).resolve()
    if not (jdk / "lib/server/libjvm.so").is_file():
        parser.error("JDK does not supply lib/server/libjvm.so")
    cxx, cc = shlex.split(args.cxx), shlex.split(args.cc)
    for label, command in (("CXX", cxx), ("CC", cc), ("JVM", [str(jdk / "bin/java")])):
        print(label, json.dumps(command), flush=True)
        subprocess.run(command + ["--version"], check=True)
    fixtures = ("native_namespace_jvm_host.cc", "native_namespace_fixture.c",
                "NativeNamespaceRegistryTest.java", "NativeLibraryPolicyTest.java", "native_namespace_lock_observer.h",
                "elf_loader_host_platform.cc", "elf_loader_host_arm64.cc")
    with tempfile.TemporaryDirectory(prefix="gesher-native-namespaces-") as tmp:
        folder = Path(tmp)
        elf, fixture = host.snapshot(folder, None, fixtures)
        source = folder / "source"
        for name in ("native_loader.cc", "native_library_catalog.cc"):
            destination = source / "compat/art" / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(host.ROOT / "compat/art" / name, destination)
        shutil.copytree(host.ROOT / "compat/art/include", source / "compat/art/include")
        include = source / "public/gesher"
        include.mkdir(parents=True)
        for path in (host.ROOT / "compat/posix/include/gesher/elf_dlopen.h",
                     host.ROOT / "compat/bionic/include/gesher/bionic_shim.h"):
            shutil.copy2(path, include / path.name)
        for path in sorted(source.rglob("*")):
            if path.is_file() and "compat/elf/" not in path.as_posix():
                print("ADAPTER_SOURCE_SHA256", path.relative_to(source), host.digest(path), flush=True)
        graph = folder / "graph"
        provider = graph / "link-only"
        provider.mkdir(parents=True)
        flags = ["-shared", "-fPIC", "-nostdlib", "-Wl,--hash-style=both"]
        fixture_c = str(fixture / "native_namespace_fixture.c")
        host.run(cc + flags + ["-DEVENTS_PROVIDER", "-Wl,-soname,libregistry_events.so", fixture_c,
                               "-o", str(provider / "libregistry_events.so")], "BUILD valid event-provider link input")
        for relative, soname, value in (("alpha", "libsame.so", 100), ("beta", "libsame.so", 200),
                ("race", "librace.so", 500),
                ("device/vendor/lib64", "libpresent.vendor.so", 300),
                ("device/system/lib64", "libpresent.fixture.so", 400)):
            directory = graph / relative
            directory.mkdir(parents=True, exist_ok=True)
            host.run(cc + flags + ["-DFIXTURE_VALUE=" + str(value), "-Wl,-soname," + soname,
                     fixture_c, "-L", str(provider), "-Wl,--no-as-needed", "-lregistry_events",
                     "-o", str(directory / soname)], "BUILD valid ELF " + relative + "/" + soname)
        os.link(graph / "alpha/libsame.so", graph / "alpha/alias.so")
        for partition in ("system", "vendor"):
            (graph / "device" / partition / "etc").mkdir(parents=True)
        (graph / "device/vendor/etc/public.libraries.txt").write_text(
            "# Original real ELF fixture\nlibpresent.vendor.so 64 nopreload\n"
            "libmissing.vendor.so\nlibonly32.vendor.so 32\n")
        (graph / "device/system/etc/public.libraries-fixture.txt").write_text(
            "libpresent.fixture.so nopreload nopreload\nlibignored32.so 32\n")
        classes = folder / "classes"
        classes.mkdir()
        host.run([str(jdk / "bin/javac"), "-d", str(classes),
                  str(fixture / "NativeNamespaceRegistryTest.java")], "COMPILE actual Java classloader controls")
        runtime_classpath = [str(classes)]
        if args.api_cache:
            api = args.api_cache.resolve()
            dependencies = args.generated_dependencies.resolve()
            runtime_classpath += [str(api / "framework-api"), str(api / "core-api"), str(dependencies)]
            java_sources = []
            for name in ("com/gesher/server/NativeLibraryCatalog.java", "com/gesher/server/CompatChanges.java",
                         "com/gesher/app/NativeLibs.java"):
                original = host.ROOT / "app/java" / name
                copied = source / "app/java" / name
                copied.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(original, copied)
                java_sources.append(str(copied))
                print("JAVA_PRODUCTION_SOURCE_SHA256", name, host.digest(copied), flush=True)
            print("GENERATED_DEPENDENCIES_SHA256", host.digest(dependencies), flush=True)
            host.run([str(jdk / "bin/javac"), "-source", "17", "-target", "17", "-cp",
                      os.pathsep.join(runtime_classpath), "-d", str(classes)] + java_sources +
                     [str(fixture / "NativeLibraryPolicyTest.java")], "COMPILE production Java native dependency policy")
        includes = ["-I", str(fixture / "elf_loader_host_platform"), "-I", str(elf / "include"),
                    "-I", str(elf), "-I", str(fixture / "arm64-include"),
                    "-I", str(source / "compat/art/include"), "-I", str(source / "public"),
                    "-I", str(jdk / "include"), "-I", str(jdk / "include/linux")]
        compile_flags = ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions", "-pthread"]
        print("COMPILE_FLAGS", json.dumps(compile_flags + includes), flush=True)
        # native_namespace_jvm_host.cc includes the complete unchanged loader
        # body so it can inspect real stored links and reset state directly.
        sources = [elf / name for name in host.PRODUCTION_UNITS if name != "elf_loader.cc"]
        sources += [fixture / "elf_loader_host_platform.cc", fixture / "elf_loader_host_arm64.cc",
                    source / "compat/art/native_loader.cc", source / "compat/art/native_library_catalog.cc",
                    fixture / "native_namespace_jvm_host.cc"]
        objects = []
        for index, path in enumerate(sources):
            output = folder / (str(index) + ".o")
            transport = (["-Ddlerror=gesher_registry_host_dlerror", "-include",
                          str(fixture / "native_namespace_lock_observer.h")]
                         if path.name == "native_loader.cc" else [])
            host.run(cxx + compile_flags + includes + transport + ["-c", str(path), "-o", str(output)],
                     "COMPILE production/host boundary " + path.name)
            objects.append(str(output))
        runner = folder / "native-jvm"
        host.run(cxx + objects + ["-pthread", "-L", str(jdk / "lib/server"), "-ljvm", "-ldl",
                "-Wl,-rpath," + str(jdk / "lib/server"), "-o", str(runner)], "LINK real JVM + production adapter/ELF")
        environment = os.environ.copy()
        environment["ANDROID_ROOT"] = str(graph / "device/system")
        environment["ANDROID_DATA"] = str(graph)
        command = [str(runner), str(graph), os.pathsep.join(runtime_classpath)]
        if args.api_cache:
            command.append("java-policy")
        started = time.monotonic()
        host.run(command, "RUN -Xcheck:jni production classloader registry and requested Java policy", env=environment)
        print("JVM_RUN_SECONDS", round(time.monotonic() - started, 6), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
