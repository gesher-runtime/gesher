// gesher: real compiler-generated ARM/Thumb softfp code for executor testing.
// No Bionic/JNI imports: this is not an Android application compatibility test.
static volatile int initial;
static int value = 19;
static int* volatile indirect = &value;

__attribute__((constructor)) static void initialize(void) { initial = 23; }

int fixture_add(int a, int b) { return a + b + initial + *indirect; }
long long fixture_wide(int prefix, long long a, long long b, int suffix) {
  return a + b + prefix + suffix;
}
double fixture_double(int a, double b, int c, double d) {
  return a + b + c + d;
}
float fixture_float(float a, float b) { return a + b; }
unsigned fixture_memory(unsigned* values, unsigned count) {
  unsigned sum = 0;
  for (unsigned i = 0; i < count; ++i) sum += values[i];
  values[0] = sum;
  return sum;
}
extern int host_adjust(int);
int fixture_import(int value) { return host_adjust(value) + 5; }
