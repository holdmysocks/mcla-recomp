// PS5 port milestone P4: the recompiled game linked with the runtime, started
// without graphics.
//
// The desktop host goes through rex::ReXApp, which is built around a window
// and an event loop. There is neither here yet (presentation is P5), so this
// drives rex::Runtime directly, in the same order ReXApp does: setup, XEX
// load, guest heap, main thread. With no graphics system the game is expected
// to stop when it reaches its video setup; the point of this milestone is that
// the guest entry point runs and kernel calls are logged.
//
// Runs as a payload through the ELF loader, so standard output is the loader's
// socket. Every stage is announced before it starts (see
// docs/ps5-port-plan.md for why).
//
// Stops after MCLA_STAGE, so the first runs on the console can go one stage at
// a time:
//   1  construct the runtime and Setup() (memory, kernel, file systems)
//   2  ... and load the XEX image into guest memory
//   3  ... and create the guest heap and the suspended main thread
//   4  ... and resume the main thread, then watch it for MCLA_RUN_SECONDS

#include "generated/default/mcla_init.h"

#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/kernel/crt/heap.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>

#ifndef MCLA_STAGE
#define MCLA_STAGE 4
#endif
#ifndef MCLA_RUN_SECONDS
#define MCLA_RUN_SECONDS 20
#endif
#ifndef MCLA_PS5_ROOT
#define MCLA_PS5_ROOT "/data/mcla"
#endif

REXCVAR_DECLARE(uint32_t, rexcrt_heap_size_mb);

std::unique_ptr<rex::system::IAudioSystem> CreateMclaAudioSystem(
    rex::runtime::FunctionDispatcher* function_dispatcher);

// --- Early crash reporter -------------------------------------------------------
//
// Installed from a constructor that runs ahead of the runtime's static
// objects. Prints the signal, fault address and instruction pointer straight
// to the socket and exits; it never returns into the context. The runtime's
// own fault handler takes over SIGSEGV/SIGBUS/SIGILL later and hands a fault
// nobody claims back to this one.

namespace {

void EarlyWrite(const char* text) {
  (void)!write(1, text, std::strlen(text));
}

void EarlyHex(const char* label, uint64_t value) {
  char buffer[96];
  static const char digits[] = "0123456789abcdef";
  size_t n = 0;
  while (*label) buffer[n++] = *label++;
  buffer[n++] = '0';
  buffer[n++] = 'x';
  for (int shift = 60; shift >= 0; shift -= 4) buffer[n++] = digits[(value >> shift) & 0xF];
  buffer[n++] = '\n';
  (void)!write(1, buffer, n);
}

int EarlyAnchor() { return 0; }

void EarlyCrash(int signal_number, siginfo_t* info, void* context) {
  EarlyWrite("CRASH\n");
  EarlyHex("  signal ", static_cast<uint64_t>(signal_number));
  EarlyHex("  fault address ", reinterpret_cast<uint64_t>(info->si_addr));
  // The machine context is 0x40 bytes into the signal context on firmware
  // 13.42, whatever the SDK's ucontext_t says.
  auto* machine = reinterpret_cast<mcontext_t*>(static_cast<uint8_t*>(context) + 0x40);
  const uint64_t anchor = reinterpret_cast<uint64_t>(&EarlyAnchor);
  EarlyHex("  instruction pointer ", static_cast<uint64_t>(machine->mc_rip));
  EarlyHex("  address of EarlyAnchor ", anchor);
  EarlyHex("  rsp ", static_cast<uint64_t>(machine->mc_rsp));
  // Return addresses on the stack, for a rough backtrace. The image is larger
  // than the probes', so accept anything within 512 MiB of this function.
  const uint64_t* stack = reinterpret_cast<const uint64_t*>(machine->mc_rsp);
  int printed = 0;
  for (int i = 0; i < 1024 && printed < 16; ++i) {
    const uint64_t word = stack[i];
    // A title is loaded at 0x400000, below the range: do not let the lower bound wrap.
    const uint64_t low = anchor > 0x20000000 ? anchor - 0x20000000 : 0x1000;
    if (word > low && word < anchor + 0x20000000) {
      EarlyHex("  stack code pointer ", word);
      ++printed;
    }
  }
  _exit(100 + signal_number);
}

__attribute__((constructor(101))) void InstallEarlyCrashReporter() {
  EarlyWrite("early constructor: installing the crash reporter\n");
  struct sigaction action;
  std::memset(&action, 0, sizeof action);
  action.sa_sigaction = EarlyCrash;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  for (int signal_number : {SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGFPE}) {
    sigaction(signal_number, &action, nullptr);
  }
}

void Line(const char* format, ...) {
  char text[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof text, format, args);
  va_end(args);
  std::printf("%s\n", text);
  std::fflush(stdout);
}

// Printed before an operation, so the last "NEXT" line names what was running
// if the output stops.
#define NEXT(...) Line("NEXT " __VA_ARGS__)

int Finish(const char* what, int code) {
  Line("mcla-ps5 stops: %s", what);
  rex::FlushLogging();
  std::fflush(stdout);
  // No teardown: guest threads may be running, and the runtime's shutdown
  // path is not something to try for the first time on the console.
  _exit(code);
}

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  Line("mcla-ps5 starts, pid %d, stage %d", getpid(), MCLA_STAGE);
  alarm(MCLA_RUN_SECONDS + 120);

  const std::filesystem::path root = MCLA_PS5_ROOT;
  const std::filesystem::path game_root = root / "game";
  const std::filesystem::path user_root = root / "user";
  const std::filesystem::path cache_root = root / "cache";

  NEXT("check the game folder %s", game_root.c_str());
  std::error_code ec;
  if (!std::filesystem::is_regular_file(game_root / "default.xex", ec)) {
    return Finish("default.xex is not in the game folder", 2);
  }
  std::filesystem::create_directories(user_root, ec);
  std::filesystem::create_directories(cache_root, ec);

  NEXT("initialise cvars and logging (standard output)");
  char program[] = "mcla";
  char* arguments[] = {program, nullptr};
  rex::cvar::Init(1, arguments);
  rex::InitLoggingEarly();
  rex::LogConfig log_config;
  log_config.log_to_console = true;
#ifdef MCLA_LOG_LEVEL
  log_config.default_level = spdlog::level::from_str(MCLA_LOG_LEVEL);
#endif
  log_config.flush_level = spdlog::level::trace;
  rex::InitLogging(log_config);

  NEXT("construct rex::Runtime");
  auto runtime = std::make_unique<rex::Runtime>(game_root, user_root, std::filesystem::path(),
                                                cache_root, std::filesystem::path());

  rex::RuntimeConfig config;
  config.audio_factory = &CreateMclaAudioSystem;
  config.kernel_init = rex::kernel::InitializeKernel;
  // No graphics system and no input system: P5 and P7.

  rex::PPCImageInfo image = PPCImageConfig;
  NEXT("Runtime::Setup (guest memory, function table, kernel state, file systems)");
  auto status = runtime->Setup(image, std::move(config));
  if (XFAILED(status)) {
    Line("Runtime::Setup failed: %08X", static_cast<unsigned>(status));
    return Finish("setup failed", 3);
  }
  if (image.register_modules) {
    NEXT("register guest modules");
    image.register_modules(runtime->kernel_state());
  }
  Line("PASS Runtime::Setup");
  if (MCLA_STAGE <= 1) return Finish("stage 1 complete", 0);

  NEXT("Runtime::LoadXexImage game:\\default.xex");
  status = runtime->LoadXexImage("game:\\default.xex");
  if (XFAILED(status)) {
    Line("LoadXexImage failed: %08X", static_cast<unsigned>(status));
    return Finish("XEX load failed", 4);
  }
  Line("PASS XEX image loaded, title id %08X",
       static_cast<unsigned>(runtime->kernel_state()->title_id()));
  if (MCLA_STAGE <= 2) return Finish("stage 2 complete", 0);

  if (image.rexcrt_heap) {
    NEXT("create the guest heap (%u MiB)", static_cast<unsigned>(REXCVAR_GET(rexcrt_heap_size_mb)));
    if (!rex::kernel::crt::InitHeap(REXCVAR_GET(rexcrt_heap_size_mb), runtime->memory())) {
      return Finish("guest heap creation failed", 5);
    }
  }
  runtime->file_system()->RegisterSymbolicLink("t:", "\\Device\\Harddisk0\\Partition1");

  NEXT("Runtime::PrepareModuleLaunch (suspended main guest thread)");
  auto main_thread = runtime->PrepareModuleLaunch();
  if (!main_thread) {
    return Finish("could not create the main guest thread", 6);
  }
  Line("PASS main guest thread created");
  if (MCLA_STAGE <= 3) return Finish("stage 3 complete", 0);

  NEXT("resume the main guest thread; guest code runs from here");
  main_thread->Resume();

  for (int second = 1; second <= MCLA_RUN_SECONDS; ++second) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    Line("alive: %d s", second);
  }
  return Finish("run time reached", 0);
}
