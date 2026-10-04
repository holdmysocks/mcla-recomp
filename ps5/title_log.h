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
// Call MclaTitleLogConnect() first thing in a constructor(101) function.

#pragma once

#ifdef MCLA_TITLE

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
  dup2(connection, 1);
  dup2(connection, 2);
  static const char hello[] = "title log connected\n";
  (void)!write(1, hello, sizeof hello - 1);
}

#else

inline void MclaTitleLogConnect() {}

#endif
