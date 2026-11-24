// gesher: a stand-in libart.so for apps whose packer hooks ART by symbol
// (compat/bionic/art_shim.cc, docs/decisions/0086-packer-art-hooks.md).
//
// Ijiami (爱加密) ships the app's dex files with their method bodies left out.
// Its native shell finds libart.so in /proc/self/maps, reads the symbol of
// art::ClassLinker::LoadClass from it and hooks that function: before ART
// reads a class, the hook writes the class's method bodies back into the dex
// file. Gesher's ART is part of dalvikvm.exe, not an ELF libart.so, so the
// shell finds nothing to hook, says nothing, and ART later rejects the empty
// methods (VerifyError: "Constructor returning without calling superclass
// constructor").
//
// The stand-in is a small ELF object written and loaded here: it exports that
// symbol as a System V function that does nothing. ART calls it at the start
// of ClassLinker::LoadClass with the device's arguments (patches/art/0800);
// when the packer has hooked it, the packer's code runs there, as on a device.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace gesher::bionic {

// The symbol the stand-in exports: art::ClassLinker::LoadClass(Thread*,
// const DexFile&, const dex::ClassDef&, Handle<mirror::Class>).
extern const char kArtLoadClassSymbol[];

// Is `path` the native shell of a packer that hooks ART by symbol (its base
// name: Ijiami's libexec.so, libexecmain.so and their _x86 variants, NetEase
// Yidun's libnesec.so)? Which stand-in it gets depends on its machine.
bool IsArtHookingPacker(const char* path);

// The ELF machine (e_machine) of the shells named `path` that a stand-in
// covers, or 0 when none: EM_X86_64 for Ijiami's names, EM_AARCH64 for NetEase
// Yidun's libnesec.so (ADR 0086). A shell of another machine gets no stand-in.
uint16_t ArtHookingPackerMachine(const char* path);

// The stand-ins' directories under `dir` are named "<kind>-<16 hex digits>"
// (a 64-bit hash of their contents), kind being one of these.
extern const char kArtShimKindX86_64[];
extern const char kArtShimKindArm64[];

// Removes stand-in directories under `dir` an earlier Gesher left: those of
// `keep`'s kind other than `keep`, and the 8-hex-digit names used before
// kinds, when untouched for `min_age`. A file a process still has mapped is
// not deleted (Windows refuses), and that directory stays; failures are
// ignored. The loaders call it after loading their stand-ins, and mark the
// directory they use as written when they start using it.
void PruneArtShimDirectories(const std::string& dir, const std::string& keep,
                             std::chrono::seconds min_age);

// The stand-in libart.so's bytes (an ELF64 x86-64 ET_DYN object: one
// read-execute PT_LOAD from offset 0, .dynsym with DT_HASH and DT_GNU_HASH,
// and section headers with .symtab, as a parser of either finds it).
std::string ArtShimImage();

// Writes the stand-in under `dir` (a directory named after its contents, the
// file named libart.so, as /proc/self/maps then names it), loads it with the
// ELF loader and returns its LoadClass entry point; nullptr with *error set on
// failure. *path receives the file's path. Loads it once per process: later
// calls return the same entry point.
void* LoadArtShim(const std::string& dir, std::string* path, std::string* error);

// The arm64 stand-in, for a shell that runs translated (NetEase Yidun's
// arm64-v8a libnesec.so). Besides LoadClass, that shell calls
// art::Runtime::DisableVerifier() on the Runtime before it opens its dex files
// from memory (ART then opens them without verifying them, as
// OatFileManager::OpenDexFilesFromOat does when verification is disabled) and
// reads OatHeader::kOatVersion. The stand-in exports them too; its Runtime
// functions forward through GOT slots to the host functions named below, which
// the bridge registers (compat/bionic/jni_bridge.cc).
extern const char kArtDisableVerifierSymbol[];
extern const char kArtIsVerificationEnabledSymbol[];
extern const char kArtIsVerificationSoftFailSymbol[];
extern const char kArtOatVersionSymbol[];
// art::Runtime::instance_, the Runtime pointer the shell calls them on.
extern const char kArtRuntimeInstanceSymbol[];
// art::DexFileLoader::OpenCommon (Android 16's, with a DexFileContainer), which
// the shell hooks so that the dex files it opens from memory are opened where
// its own buffers are (its LoadClass hook finds a dex file by that address).
// On a device it is libdexfile.so's, not libart.so's, and the shell looks for
// it only there (in /proc/self/maps), so a second stand-in, libdexfile.so,
// exports it. ART calls it through that stand-in's entry below
// (patches/art/0803): the entry makes the device's call, with the result's
// storage in X8, so a hook runs as there; the stand-in's own body calls ART's
// function back through the import named below.
extern const char kArtOpenCommonSymbol[];
extern const char kArtOpenCommonEntrySymbol[];
extern const char kArtShimOpenCommonImport[];
extern const char kArtShimDisableVerifierImport[];
extern const char kArtShimIsVerificationEnabledImport[];
extern const char kArtShimIsVerificationSoftFailImport[];

// Its bytes: an ELF64 AArch64 ET_DYN object, a read-execute PT_LOAD from
// offset 0 as ArtShimImage's, and a read-write page holding the GOT.
// `oat_version` is the 4 bytes of OatHeader::kOatVersion it exports.
std::string ArtShimImageArm64(const std::string& oat_version);
// The stand-in libdexfile.so: OpenCommon and its entry, as an object of the
// same form.
std::string DexFileShimImageArm64();

// The host (System V) functions the arm64 stand-ins' imports are bound to,
// by the import names above.
struct ArtShimArm64Imports {
  void* disable_verifier = nullptr;           // kArtShimDisableVerifierImport
  void* is_verification_enabled = nullptr;    // kArtShimIsVerificationEnabledImport
  void* is_verification_soft_fail = nullptr;  // kArtShimIsVerificationSoftFailImport
  void* open_common = nullptr;                // kArtShimOpenCommonImport
};

// As LoadArtShim, for the arm64 stand-in: binds its imports to `imports`, sets
// its Runtime::instance_ to `runtime` and returns the guest address of its
// LoadClass, and in *open_common_entry that of its OpenCommon entry
// (gesher::arm64::HostEntry gives ART a System V entry to each). A failure
// after libdexfile.so loaded keeps that object for the next call, which loads
// only what is missing (an object is never loaded twice).
void* LoadArtShimArm64(const std::string& dir, const std::string& oat_version, void* runtime,
                       const ArtShimArm64Imports& imports, void** open_common_entry,
                       std::string* path, std::string* error);

// The ELF loader's bionic resolver asks this first (compat/bionic/shim.cc): one
// of the stand-ins' imports, while LoadArtShimArm64 loads them on this thread;
// nullptr for any other name, thread or time. The imports are in no symbol
// table, so neither an app's dlsym nor any other object's imports reach them.
void* ResolveArtShimImport(const char* name);

}  // namespace gesher::bionic
