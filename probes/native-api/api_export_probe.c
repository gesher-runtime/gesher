// This fixture looks up names only; it never invokes resolved API pointers.
#include <jni.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

JNIEXPORT jint JNICALL Java_gesher_ApiExportInventory_scan(JNIEnv* env, jclass cls,
    jstring manifest, jstring result) {
  (void)cls;
  const char* input_path = (*env)->GetStringUTFChars(env, manifest, NULL);
  if (input_path == NULL) return -1;
  FILE* input = fopen(input_path, "r");
  (*env)->ReleaseStringUTFChars(env, manifest, input_path);
  if (input == NULL) return -2;
  const char* output_path = (*env)->GetStringUTFChars(env, result, NULL);
  if (output_path == NULL) { fclose(input); return -3; }
  FILE* output = fopen(output_path, "w");
  (*env)->ReleaseStringUTFChars(env, result, output_path);
  if (output == NULL) { fclose(input); return -4; }
  int checked = 0;
  char line[2048], previous_library[128] = "";
  void* library = NULL;
  while (fgets(line, sizeof(line), input) != NULL) {
    char* separator = strchr(line, '\t');
    if (separator == NULL) { checked = -5; break; }
    *separator++ = '\0';
    separator[strcspn(separator, "\r\n")] = '\0';
    if (strlen(line) >= sizeof(previous_library) || !*separator) { checked = -6; break; }
    if (strcmp(line, previous_library) != 0) {
      if (library != NULL) dlclose(library);
      strcpy(previous_library, line);
      library = dlopen(line, RTLD_NOW | RTLD_LOCAL);
      fprintf(output, "OPEN\t%s\t%d\n", line, library != NULL);
    }
    int global_found = dlsym(RTLD_DEFAULT, separator) != NULL;
    int handle_found = library != NULL && dlsym(library, separator) != NULL;
    fprintf(output, "SYMBOL\t%s\t%s\t%d\t%d\n", line, separator, global_found, handle_found);
    ++checked;
  }
  if (library != NULL) dlclose(library);
  fprintf(output, "END\t%d\n", checked);
  if (ferror(input) || ferror(output)) checked = -7;
  fclose(input);
  if (fclose(output) != 0) checked = -8;
  return checked;
}
