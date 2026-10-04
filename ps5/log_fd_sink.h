// An spdlog sink that writes to a file descriptor.
//
// The runtime logs to standard output. In a payload that is the loader
// socket; in a title it is nowhere, and it cannot be redirected (dup2 onto
// descriptor 1 is refused, see title_log.h). Adding this sink with
// rex::AddSink() sends the runtime's log to g_mcla_log_fd instead.

#pragma once

#include <unistd.h>

#include <mutex>

#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>

class MclaFdSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  explicit MclaFdSink(int descriptor) : descriptor_(descriptor) {}

 protected:
  void sink_it_(const spdlog::details::log_msg& message) override {
    spdlog::memory_buf_t formatted;
    formatter_->format(message, formatted);
    (void)!write(descriptor_, formatted.data(), formatted.size());
  }
  void flush_() override {}

 private:
  int descriptor_;
};
