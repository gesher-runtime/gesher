/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
// Gesher's narrow libnativeloader host adapter. ART keeps ownership of JNI
// paths, classloaders, JNI_OnLoad results and unloading decisions. This layer
// supplies the classloader's native search/visibility context to the ELF loader.
#include <gesher/native_loader.h>

#include <gesher/bionic_shim.h>
#include <gesher/elf_dlopen.h>
#include <gesher/elf_loader.h>
#include <gesher/native_library_catalog.h>

#include <dlfcn.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace gesher::nativeloader {
namespace {

struct Entry {
  jweak class_loader;
  elf::Namespace* name_space;
};

std::mutex g_mutex;
std::vector<Entry> g_entries;
std::map<std::string, elf::Namespace*> g_file_namespaces;
elf::Namespace* g_main_namespace = nullptr;
size_t g_sequence = 0;

bool Utf(JNIEnv* env, jstring value, std::string* output) {
  output->clear();
  if (value == nullptr) return true;
  const char* chars = env->GetStringUTFChars(value, nullptr);
  if (chars == nullptr) return false;
  *output = chars;
  env->ReleaseStringUTFChars(value, chars);
  return true;
}

std::vector<std::string> Split(const std::string& input) {
  std::vector<std::string> result;
  size_t start = 0;
  while (start < input.size()) {
    size_t end = input.find(':', start);
    if (end == std::string::npos) end = input.size();
    if (end != start) result.push_back(input.substr(start, end - start));
    start = end + 1;
  }
  return result;
}

// getParent is a Java invocation. Do it, and all string conversion/config I/O,
// before taking the registry lock. The references keep each parent alive while
// its weak identity is compared; local references are released on every path.
struct Parents {
  JNIEnv* env;
  std::vector<jobject> objects;
  ~Parents() {
    for (jobject object : objects) env->DeleteLocalRef(object);
  }
  bool Read(jobject class_loader) {
    jclass type = env->FindClass("java/lang/ClassLoader");
    if (type == nullptr) return false;
    jmethodID get_parent = env->GetMethodID(type, "getParent", "()Ljava/lang/ClassLoader;");
    env->DeleteLocalRef(type);
    if (get_parent == nullptr) return false;
    jobject object = class_loader;
    while (object != nullptr) {
      object = env->CallObjectMethod(object, get_parent);
      if (env->ExceptionCheck()) return false;
      if (object != nullptr) objects.push_back(object);
    }
    return true;
  }
};

elf::Namespace* FindLocked(JNIEnv* env, jobject class_loader) {
  // A cleared weak reference must not match the boot loader's null identity.
  if (class_loader == nullptr) return nullptr;
  for (auto it = g_entries.begin(); it != g_entries.end();) {
    if (env->IsSameObject(it->class_loader, nullptr)) {
      env->DeleteWeakGlobalRef(it->class_loader);
      it = g_entries.erase(it);
    } else {
      if (env->IsSameObject(it->class_loader, class_loader)) return it->name_space;
      ++it;
    }
  }
  return nullptr;
}

bool IsVendorDex(const std::string& dex_path, std::string* error) {
  int domain = -1;
  for (const std::string& path : Split(dex_path)) {
    const bool vendor = path.compare(0, 8, "/vendor/") == 0 ||
        path.compare(0, 15, "/system/vendor/") == 0;
    const bool product = path.compare(0, 9, "/product/") == 0 ||
        path.compare(0, 16, "/system/product/") == 0;
    const int next = vendor ? 1 : product ? 2 : 0;
    if (next != 0 && domain > 0 && domain != next) {
      *error = "Classloader dex paths span different native API domains";
      return false;
    }
    if (next != 0) domain = next;
  }
  return domain == 1;
}

elf::Namespace* Create(JNIEnv* env, int32_t target_sdk, jobject class_loader,
                        bool shared, jstring dex_path_j, jstring library_path_j,
                        jstring permitted_path_j, jstring uses_j, bool allow_existing,
                        std::string* error) {
  if (class_loader == nullptr) {
    *error = "The boot classloader has no app namespace";
    return nullptr;
  }
  Initialize();
  std::string dex_path, library_path, permitted_path, uses;
  if (!Utf(env, dex_path_j, &dex_path) || !Utf(env, library_path_j, &library_path) ||
      !Utf(env, permitted_path_j, &permitted_path) || !Utf(env, uses_j, &uses)) {
    *error = "Cannot read classloader namespace arguments";
    return nullptr;
  }
  const bool vendor = IsVendorDex(dex_path, error) && !shared;
  if (!error->empty()) return nullptr;
  Parents parents{env, {}};
  if (!parents.Read(class_loader)) {
    *error = "Cannot inspect the classloader parent chain";
    return nullptr;
  }
  const auto providers = elf::AvailableProviders();
  std::vector<PublicLibraryFile> public_files;
  if (!ReadPublicLibraryFiles(SystemRoot(), &public_files, error)) return nullptr;
  const auto requested = Split(uses);
  // Null is the pinned ART policy for an unknown custom classloader. Empty
  // means that an ordinary framework-created loader declares no device libs.
  const bool all = target_sdk <= 30 || uses_j == nullptr ||
      std::find(requested.begin(), requested.end(), "ALL") != requested.end();
  auto visible = [&](const std::string& name, elf::ProviderVisibility visibility) {
    return visibility == elf::ProviderVisibility::kNdkPublic ||
        (visibility == elf::ProviderVisibility::kDevicePublic &&
          (all || std::find(requested.begin(), requested.end(), name) != requested.end()));
  };
  std::vector<std::string> ndk_names, builtin_names;
  for (const auto& provider : providers) {
    if (provider.visibility == elf::ProviderVisibility::kNdkPublic) {
      ndk_names.push_back(provider.soname);
    }
    if (visible(provider.soname, provider.visibility)) builtin_names.push_back(provider.soname);
  }

  // ELF constructors run under the loader's recursive operation lock. Take
  // that lock before the registry lock so a concurrent creation cannot hold
  // the registry while waiting for a constructor which reenters it.
  elf::ScopedLoaderOperation operation;
  std::lock_guard<std::mutex> guard(g_mutex);
  if (elf::Namespace* found = FindLocked(env, class_loader)) {
    if (allow_existing) return found;
    *error = "A native namespace already exists for this classloader";
    return nullptr;
  }
  elf::NamespaceOptions options;
  options.name = "classloader-" + std::to_string(++g_sequence) + (shared ? "-shared" : "");
  options.target_sdk_version = target_sdk;
  options.isolated = true;
  options.shared = shared;
  options.exempt_list_enabled = target_sdk < 24;
  // NativeLoaderNamespace::Create passes library_path as the namespace's
  // default path, with no LD_LIBRARY_PATH override. Requester RUNPATH therefore
  // retains its position before this classloader search path.
  options.default_library_paths = Split(library_path);
  // Android permits custom ClassLoaders' System.load paths under /data and
  // /mnt/expand. ANDROID_DATA identifies Gesher's corresponding device root.
  options.permitted_paths = {"/data", "/mnt/expand"};
  const char* data = std::getenv("ANDROID_DATA");
  if (data != nullptr && *data != '\0') options.permitted_paths.emplace_back(data);
  const auto permitted = Split(permitted_path);
  options.permitted_paths.insert(options.permitted_paths.end(), permitted.begin(), permitted.end());
  for (jobject parent : parents.objects) {
    if ((options.parent = FindLocked(env, parent)) != nullptr) break;
  }
  // Allocate the JNI identity first: an OOM must not publish a namespace with
  // no classloader owner. Namespace objects themselves are process-lifetime
  // while the ELF loader retains objects; unloading/GC is a separate contract.
  jweak identity = env->NewWeakGlobalRef(class_loader);
  if (identity == nullptr) {
    *error = "Cannot retain the classloader's weak JNI identity";
    return nullptr;
  }
  elf::Namespace* ns = elf::CreateNamespace(options, error);
  auto failed = [&]() -> elf::Namespace* {
    env->DeleteWeakGlobalRef(identity);
    return nullptr;
  };
  if (ns == nullptr || (!builtin_names.empty() &&
      !elf::LinkNamespaces(ns, elf::DefaultNamespace(), builtin_names, error))) {
    return failed();
  }
  // Materialize actual file domains before adding links. A device library may
  // depend on a public NDK ELF file from another domain, not a virtual builtin.
  std::map<std::string, std::vector<std::string>> ndk_file_links, app_file_links;
  std::vector<std::string> file_domains;
  for (const auto& file : public_files) {
    elf::Namespace*& file_ns = g_file_namespaces[file.directory];
    if (file_ns == nullptr) {
      elf::NamespaceOptions file_options;
      file_options.name = "public-files-" + file.directory;
      file_options.default_library_paths = {file.directory};
      file_options.permitted_paths = {file.directory};
      file_ns = elf::CreateNamespace(file_options, error);
      if (file_ns == nullptr) return failed();
    }
    if (std::find(file_domains.begin(), file_domains.end(), file.directory) == file_domains.end()) {
      file_domains.push_back(file.directory);
    }
    if (file.visibility == elf::ProviderVisibility::kNdkPublic) {
      ndk_file_links[file.directory].push_back(file.soname);
    }
    if (visible(file.soname, file.visibility) &&
        !(vendor && file.visibility == elf::ProviderVisibility::kDevicePublic && !file.vendor)) {
      app_file_links[file.directory].push_back(file.soname);
    }
  }
  for (const std::string& directory : file_domains) {
    elf::Namespace* file_ns = g_file_namespaces.at(directory);
    if (!ndk_names.empty() &&
        !elf::LinkNamespaces(file_ns, elf::DefaultNamespace(), ndk_names, error)) return failed();
    for (const auto& [dependency_directory, names] : ndk_file_links) {
      if (dependency_directory == directory) continue;
      if (!elf::LinkNamespaces(file_ns, g_file_namespaces.at(dependency_directory), names, error)) {
        return failed();
      }
    }
  }
  for (const auto& [directory, names] : app_file_links) {
    if (!elf::LinkNamespaces(ns, g_file_namespaces.at(directory), names, error)) return failed();
  }
  if (g_main_namespace == nullptr && !library_path.empty()) {
    if (!elf::SetAnonymousNamespace(ns, error)) return failed();
    g_main_namespace = ns;
  }
  g_entries.push_back({identity, ns});
  return ns;
}

const char* Visibility(elf::ProviderVisibility visibility) {
  switch (visibility) {
    case elf::ProviderVisibility::kNdkPublic: return "ndk";
    case elf::ProviderVisibility::kDevicePublic: return "device";
    default: return "private";
  }
}

struct CatalogRow { std::string name; const char* visibility; const char* abi; };

jobjectArray Rows(JNIEnv* env, const std::vector<CatalogRow>& rows) {
  jclass string_class = env->FindClass("java/lang/String");
  if (string_class == nullptr) return nullptr;
  jclass row_class = env->FindClass("[Ljava/lang/String;");
  if (row_class == nullptr) { env->DeleteLocalRef(string_class); return nullptr; }
  jobjectArray result = env->NewObjectArray(static_cast<jsize>(rows.size()), row_class, nullptr);
  env->DeleteLocalRef(row_class);
  for (size_t i = 0; result != nullptr && i < rows.size(); ++i) {
    jobjectArray row = env->NewObjectArray(3, string_class, nullptr);
    if (row == nullptr) break;
    const char* values[] = {rows[i].name.c_str(), rows[i].visibility, rows[i].abi};
    for (jsize j = 0; j < 3; ++j) {
      jstring value = env->NewStringUTF(values[j]);
      if (value == nullptr) break;
      env->SetObjectArrayElement(row, j, value);
      env->DeleteLocalRef(value);
    }
    if (!env->ExceptionCheck()) env->SetObjectArrayElement(result, static_cast<jsize>(i), row);
    env->DeleteLocalRef(row);
    if (env->ExceptionCheck()) break;
  }
  env->DeleteLocalRef(string_class);
  return env->ExceptionCheck() ? nullptr : result;
}

}  // namespace

void Initialize() { bionic::InstallJniBridge(); }

void SetApplicationTargetSdkVersion(uint32_t version) {
  elf::SetDefaultTargetSdkVersion(static_cast<int>(version));
}

void Reset() {
  // ART calls ResetNativeLoader after destroying Runtime and its JavaVM. Like
  // LibraryNamespaces::Reset, discard native metadata without consulting JNI:
  // the weak-reference table already belonged to the retired VM.
  elf::ScopedLoaderOperation operation;
  std::lock_guard<std::mutex> guard(g_mutex);
  g_entries.clear();
  g_main_namespace = nullptr;
  // Bionic's anonymous namespace and live ELF identities are process-lifetime.
  // ART's registry reset does not reassign that namespace or provide a general
  // JavaVM-recreation contract. Only the pinned linker's test-only initializer
  // clears its anonymous-assignment flag.
}

jstring CreateClassLoaderNamespace(JNIEnv* env, int32_t target_sdk_version,
                                   jobject class_loader, bool is_shared, jstring dex_path,
                                   jstring library_path, jstring permitted_path,
                                   jstring uses_library_list) {
  std::string error;
  if (Create(env, target_sdk_version, class_loader, is_shared, dex_path, library_path,
              permitted_path, uses_library_list, false, &error) != nullptr) return nullptr;
  return env->ExceptionCheck() ? nullptr : env->NewStringUTF(error.c_str());
}

void* OpenNativeLibrary(JNIEnv* env, int32_t target_sdk_version, const char* path,
                        jobject class_loader, const char* /*caller_location*/, jstring library_path,
                        bool* needs_native_bridge, char** error_msg) {
  *needs_native_bridge = false;
  *error_msg = nullptr;
  if (class_loader == nullptr) {
    // ART passes null only for the boot classloader. Its actual host builtins
    // and PE modules do not acquire an application's native privileges.
    void* handle = gesher_dlopen_host(path, RTLD_NOW);
    if (handle == nullptr) {
      const char* message = dlerror();
      *error_msg = strdup(message != nullptr ? message : "Cannot load a boot native library");
    }
    return handle;
  }
  elf::Namespace* ns;
  {
    std::lock_guard<std::mutex> guard(g_mutex);
    ns = FindLocked(env, class_loader);
  }
  if (ns == nullptr) {
    std::string error;
    ns = Create(env, target_sdk_version, class_loader, false, nullptr, library_path,
                 nullptr, nullptr, true, &error);
    if (ns == nullptr) {
      *error_msg = strdup(error.c_str());
      return nullptr;
    }
  }
  const gesher_elf_load_context context{1, sizeof(gesher_elf_load_context), ns, nullptr, 0};
  // No registry lock crosses relocation, ELF constructors, JNI_OnLoad or a
  // nested Java/native callback. The namespace identity remains stable.
  void* handle = gesher_dlopen_in_namespace(path, RTLD_NOW, &context);
  if (handle == nullptr) {
    const char* message = dlerror();
    *error_msg = strdup(message != nullptr ? message : "Cannot load a classloader native library");
  }
  return handle;
}

jobjectArray BuiltinCatalog(JNIEnv* env) {
  Initialize();
  std::vector<CatalogRow> rows;
  for (const auto& provider : elf::AvailableProviders()) {
    const char* visibility = Visibility(provider.visibility);
    if ((provider.architecture_mask & 1) != 0) rows.push_back({provider.soname, visibility, "x86_64"});
    if ((provider.architecture_mask & 2) != 0) rows.push_back({provider.soname, visibility, "arm64-v8a"});
  }
  return Rows(env, rows);
}

jobjectArray PublicFileCatalog(JNIEnv* env) {
  std::vector<PublicLibraryFile> files;
  std::string error;
  if (!ReadPublicLibraryFiles(SystemRoot(), &files, &error)) {
    jclass exception = env->FindClass("java/io/IOException");
    if (exception != nullptr) { env->ThrowNew(exception, error.c_str()); env->DeleteLocalRef(exception); }
    return nullptr;
  }
  std::vector<CatalogRow> rows;
  for (const auto& file : files) {
    rows.push_back({file.soname, Visibility(file.visibility),
                    file.architecture == elf::Architecture::kX86_64 ? "x86_64" : "arm64-v8a"});
  }
  return Rows(env, rows);
}

}  // namespace gesher::nativeloader
