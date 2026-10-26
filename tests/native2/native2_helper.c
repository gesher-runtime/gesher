// native2 track: an app library the test library dlopen()s by its bare name
// (tests/native2/native2_test.cpp), from the directory it was loaded from.
static int g_value;

__attribute__((constructor)) static void Init(void) { g_value = 42; }

int native2_helper_value(void) { return g_value; }
