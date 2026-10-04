// Live log for a test that runs as an installed PS5 title.
//
// A payload's standard output is the ELF loader's socket, so its log lines are
// on the PC as they are written. A title has no such thing, and a file on the
// console does not survive the console going down (docs/ps5-port-plan.md). So
// a title built with -DMCLA_TITLE listens on a TCP port before it does
// anything else, waits for the PC to connect (ps5/title_log_client.py retries
// until it does), and makes that connection its standard output and standard
// error. Nothing else in the test changes.
//
// If nobody connects within the wait, the title exits without running the
// test: a run whose output cannot be seen is not worth the risk.
//
// Call MclaTitleLogConnect() first thing in a constructor(101) function, and
// write log lines to g_mcla_log_fd rather than to descriptor 1: in the first
// title run, text written to standard output after dup2() never reached the
// PC. The redirection is still attempted, for code that only knows standard
// output (the runtime's log), and its result is reported.

#pragma once

// Where log lines go: the loader socket (1) in a payload, the PC's connection
// in a title.
inline int g_mcla_log_fd = 1;

#ifdef MCLA_TITLE

#include <cerrno>
#include <cstdio>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#ifndef MCLA_TITLE_LOG_PORT
#define MCLA_TITLE_LOG_PORT 9099
#endif
#ifndef MCLA_TITLE_LOG_WAIT_SECONDS
#define MCLA_TITLE_LOG_WAIT_SECONDS 90
#endif

#ifdef MCLA_PLAY
// A build for playing waits for nobody: warnings, errors and a crash report go
// to a file on the console, replaced at every start (fetch it over FTP).
inline void MclaTitleLogConnect() {
  const int file = open("/data/mcla/mcla-play.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  g_mcla_log_fd = file >= 0 ? file : open("/dev/null", O_WRONLY);
}
#else
inline void MclaTitleLogConnect() {
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) _exit(90);
  int one = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in address;
  std::memset(&address, 0, sizeof address);
  address.sin_family = AF_INET;
  address.sin_port = htons(MCLA_TITLE_LOG_PORT);
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(listener, reinterpret_cast<struct sockaddr*>(&address), sizeof address) != 0) _exit(91);
  if (listen(listener, 1) != 0) _exit(92);
  struct pollfd waiting = {listener, POLLIN, 0};
  if (poll(&waiting, 1, MCLA_TITLE_LOG_WAIT_SECONDS * 1000) <= 0) _exit(93);
  const int connection = accept(listener, nullptr, nullptr);
  if (connection < 0) _exit(94);
  close(listener);
  setsockopt(connection, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  g_mcla_log_fd = connection;
  static const char hello[] = "title log connected\n";
  (void)!write(connection, hello, sizeof hello - 1);
  const int out = dup2(connection, 1);
  const int out_errno = errno;
  const int err = dup2(connection, 2);
  const int err_errno = errno;
  char report[160];
  const int length =
      std::snprintf(report, sizeof report,
                    "connection fd %d; dup2 to 1 returned %d (errno %d), to 2 returned %d (errno %d)\n",
                    connection, out, out < 0 ? out_errno : 0, err, err < 0 ? err_errno : 0);
  if (length > 0) (void)!write(connection, report, static_cast<size_t>(length));
}
#endif  // MCLA_PLAY

#else

inline void MclaTitleLogConnect() {}

#endif
