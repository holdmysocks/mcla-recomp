// klog: stream the console's kernel log to the ELF loader socket.
//
// A title that the system kills (a GPU fault, a refused system call) leaves
// nothing in its own log: no signal reaches its handlers. The reason is in the
// kernel log. This payload opens /dev/klog, forwards whatever is read to
// standard output (the loader socket, so it arrives on the PC as it happens),
// and exits after KLOG_SECONDS or when the PC closes the connection. It only
// reads.
//
// Start it, launch the title, and read the capture afterwards:
//   socat -u TCP:<console>:9021 ... < klog.elf > capture.log

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef KLOG_SECONDS
#define KLOG_SECONDS 900
#endif

int main(void) {
  static const char hello[] = "klog: streaming /dev/klog\n";
  (void)!write(1, hello, sizeof hello - 1);
  const int descriptor = open("/dev/klog", O_RDONLY | O_NONBLOCK);
  if (descriptor < 0) {
    static const char failed[] = "klog: cannot open /dev/klog\n";
    (void)!write(1, failed, sizeof failed - 1);
    return 1;
  }
  const time_t end = time(NULL) + KLOG_SECONDS;
  char buffer[4096];
  while (time(NULL) < end) {
    struct pollfd waiting = {descriptor, POLLIN, 0};
    if (poll(&waiting, 1, 500) > 0) {
      const ssize_t count = read(descriptor, buffer, sizeof buffer);
      if (count > 0 && write(1, buffer, (size_t)count) < 0) {
        break;  // the PC closed the connection
      }
    }
  }
  close(descriptor);
  static const char bye[] = "klog: done\n";
  (void)!write(1, bye, sizeof bye - 1);
  return 0;
}
