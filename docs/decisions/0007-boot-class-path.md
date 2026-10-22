# 0007 — Building the boot class path from libcore

**Status:** accepted, 2026-10-20

## Context
ART cannot start without its boot class path: `core-oj` (OpenJDK's
`java.*`), `core-libart` (`dalvik.*`, `libcore.*`, `android.system.*`) and,
later, ICU, okhttp, Conscrypt and Bouncy Castle. Soong builds them from
libcore with a `core-all` javac compile of the `java.base` module, compiled
against generated aconfig flag classes, annotation sources, ICU's intra-core
API stubs and Conscrypt's stubs, then splits and dexes the result.

## Decision
- **Build from source in CI**, with javac (JDK 21) and D8, in
  `tools/build-boot-jars.sh`. No prebuilt Android jars.
- **Soong's file lists are read, not copied.** `tools/gen/bp_files.py`
  evaluates libcore's `filegroup`s (nested references, globs,
  `exclude_srcs`), so a tag bump picks up file list changes by itself.
- **One `core-all` compile, dependencies on demand.** All of libcore is
  compiled as `java.base` with `--patch-module java.base=<roots>`, where the
  roots are the source trees of ICU, okhttp and the annotation libraries.
  javac compiles from those only the classes libcore references; they are not
  packaged. Where Soong compiles against a stub (Conscrypt's intra-core API),
  Gesher keeps a compile-only stub in `java/compile-stubs/`.
- **Split by `SourceFile`.** `tools/gen/split_classes.py` assigns each class
  file to `core-oj` or `core-libart` by the source it was compiled from, which
  also places secondary top-level classes and generated flag classes
  correctly.
- D8 dexes each jar with desugaring on (Java 21 class files use
  `ConstantDynamic`, which dex cannot express).

## Consequences
- Builds in about two minutes on a CI runner; 3,699 classes in `core-oj`,
  495 in `core-libart`.
- okhttp and Conscrypt jars are separate work. ICU4J came later: the same
  javac run compiles `android.icu` and the libcore bridge, split out as
  `core-icu4j` with ICU4J's resources
  ([0011](0011-libcore-natives.md)).
- Paths: ART separates class path entries with `:`, which Windows drive
  letters contain. For now Gesher passes drive-less absolute paths
  (`/Users/...`, resolved on the current drive); a proper path model belongs in
  gesher-base.
