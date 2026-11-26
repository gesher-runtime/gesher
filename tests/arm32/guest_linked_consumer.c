// Guest ARM32 shared object, dynamically linked to guest_linked_dependency.
extern int dependency_increment(int);
extern void dependency_mark_shutdown(void);
static int startup;
__attribute__((constructor)) static void init_consumer(void) {
  startup = dependency_increment(2);
}
int consumer_calculate(int x) { return dependency_increment(x) + startup; }
__attribute__((destructor)) static void fini_consumer(void) {
  dependency_mark_shutdown();
}
