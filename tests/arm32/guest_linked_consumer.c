// Guest ARM32 shared object dynamically linked to guest_linked_dependency.
// The dependency's constructor must run before this library's constructor.
extern int dependency_increment(int);
static int startup;
__attribute__((constructor)) static void init_consumer(void) {
  startup = dependency_increment(2);
}
int consumer_calculate(int x) { return dependency_increment(x) + startup; }
