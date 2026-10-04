/* Replacements for libc functions that are null inside an installed title.
 *
 * In a title, an import that no available system module exports is left
 * pointing at nothing, and the first call through it jumps to address 0.
 * ps5/probes/symcheck/title_main.cpp, run on the console (firmware 13.42) over
 * the 433 functions the runtime, the game and the title link import, found
 * six: isatty, link, mkstemp, pathconf, readlink, symlink. The first was hit
 * by the runtime's very first log line (spdlog asks whether standard output is
 * a terminal).
 *
 * ps5/title_build.sh binds the libc names to these with --defsym and keeps
 * the names local, as the driver project's link recipe does for its own
 * substitutes. A payload does not need them: it is given a different kernel
 * library.
 */

#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>

int mkstemps(char* template_name, int suffix_length);

int mcla_title_isatty(int descriptor) {
  (void)descriptor;
  errno = ENOTTY;
  return 0;
}

int mcla_title_link(const char* existing, const char* created) {
  (void)existing;
  (void)created;
  errno = ENOSYS;
  return -1;
}

int mcla_title_symlink(const char* target, const char* created) {
  (void)target;
  (void)created;
  errno = ENOSYS;
  return -1;
}

/* Nothing the title touches is a symbolic link as far as it can tell. */
ssize_t mcla_title_readlink(const char* path, char* buffer, size_t size) {
  (void)path;
  (void)buffer;
  (void)size;
  errno = EINVAL;
  return -1;
}

long mcla_title_pathconf(const char* path, int name) {
  (void)path;
  switch (name) {
    case _PC_NAME_MAX:
      return 255;
    case _PC_PATH_MAX:
      return 1024;
    default:
      errno = EINVAL;
      return -1;
  }
}

/* mkstemps is bound to the platform layer's own by the driver's recipe. */
int mcla_title_mkstemp(char* template_name) {
  return mkstemps(template_name, 0);
}
