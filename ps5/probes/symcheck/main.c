// symcheck: print which of a list of imported symbol names the console can
// resolve. Print-only; see build.sh for why this exists.
#include <dlfcn.h>
#include <stdio.h>

static const struct {
  const char* name;
  int is_object;
} kNames[] = {
#include "symcheck_names.inc"
};

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  const int count = (int)(sizeof(kNames) / sizeof(kNames[0]));
  int missing = 0;
  printf("symcheck: %d names\n", count);
  for (int i = 0; i < count; ++i) {
    if (!dlsym(RTLD_DEFAULT, kNames[i].name)) {
      printf("MISSING %s%s\n", kNames[i].name, kNames[i].is_object ? " (data)" : "");
      ++missing;
    }
  }
  printf("symcheck: %d missing of %d\n", missing, count);
  return 0;
}
