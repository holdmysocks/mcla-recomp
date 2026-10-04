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
//   5  graphics only (title): create the Xenos GPU system on Vulkan, its
//      device and its presenter; nothing else
//   6  presentation (title): stage 5 with a window on SDL's offscreen video
//      driver, standing for the display. Attaching the presenter to it makes
//      the VK_KHR_display surface and the swapchain; the message loop then
//      runs for MCLA_RUN_SECONDS with a repaint requested every second. No
//      guest code, so the picture is whatever the presenter clears to
//   7  the game with graphics (title): the window and graphics system of
//      stage 6 handed to the runtime, then stage 4's run with the message
//      loop on the main thread

#include "generated/default/mcla_init.h"

#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>
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
#include <vector>

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/kernel/crt/heap.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context_sdl.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include <pthread.h>

// FreeBSD's thread id call; its header is not in every SDK.
extern "C" int pthread_getthreadid_np(void);

namespace rex::arch {
uint64_t Ps5FaultCount();
}

// As a title (-DMCLA_TITLE) the log goes over a TCP connection from the PC; as
// a payload g_mcla_log_fd is standard output, the loader socket.
#include "title_log.h"
#include "log_fd_sink.h"
#ifdef MCLA_TITLE
#include "ps5_pad_input.h"
#endif

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

// The console's shell draws its launch splash over a title until the title
// asks for it to be hidden: frames presented before that are flipped but not
// seen. A plain import, and only in a title: the title converter leaves a
// weak import unbound (the call was silently skipped when it was one).
#ifdef MCLA_TITLE
extern "C" int sceSystemServiceHideSplashScreen(void);
#endif

// The Xenos GPU plugin's factory. On the other platforms the plugin is a
// shared library found by name at run time; here it is linked in.
extern "C" rex::system::IGraphicsSystem* rex_gpu_create(uint32_t abi_version,
                                                        const rex::system::GpuCreateInfo* info);

// --- Early crash reporter -------------------------------------------------------
//
// Installed from a constructor that runs ahead of the runtime's static
// objects. Prints the signal, fault address and instruction pointer straight
// to the socket and exits; it never returns into the context. The runtime's
// own fault handler takes over SIGSEGV/SIGBUS/SIGILL later and hands a fault
// nobody claims back to this one.

namespace {

void EarlyWrite(const char* text) {
  (void)!write(g_mcla_log_fd,text, std::strlen(text));
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
  (void)!write(g_mcla_log_fd,buffer, n);
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

void ReportExit() {
  EarlyWrite("EXIT: the process is leaving through exit()\n");
}

__attribute__((constructor(101))) void InstallEarlyCrashReporter() {
  MclaTitleLogConnect();
  atexit(ReportExit);
  EarlyWrite("early constructor: installing the crash reporter\n");
  struct sigaction action;
  std::memset(&action, 0, sizeof action);
  action.sa_sigaction = EarlyCrash;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  // Beyond the faults: anything else catchable that would end the process
  // without a word (a refused system call arrives as SIGSYS).
  for (int signal_number : {SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGFPE, SIGSYS, SIGTRAP, SIGTERM,
                            SIGHUP, SIGQUIT, SIGPIPE, SIGXFSZ}) {
    sigaction(signal_number, &action, nullptr);
  }
}

// --- Thread dump -------------------------------------------------------------------
//
// When the game stalls there is no debugger to ask why. This signals each of
// the runtime's threads in turn; the handler prints where that thread is
// executing and the code addresses on its stack, and returns without touching
// the context. Addresses are symbolised on the PC against the linked ELF.

constexpr int kDumpSignal = SIGXCPU;

// Set while the sampling profiler runs: the handler then writes one compact
// line per sample instead of the readable dump.
std::atomic<bool> g_profile_sampling{false};

size_t AppendHex(char* buffer, size_t n, uint64_t value) {
  static const char digits[] = "0123456789abcdef";
  int shift = 60;
  while (shift > 0 && ((value >> shift) & 0xF) == 0) shift -= 4;
  for (; shift >= 0; shift -= 4) buffer[n++] = digits[(value >> shift) & 0xF];
  return n;
}

void DumpHandler(int, siginfo_t*, void* context) {
  auto* machine = reinterpret_cast<mcontext_t*>(static_cast<uint8_t*>(context) + 0x40);
  const uint64_t anchor = reinterpret_cast<uint64_t>(&EarlyAnchor);
  const uint64_t low = anchor > 0x20000000 ? anchor - 0x20000000 : 0x1000;
  if (g_profile_sampling.load(std::memory_order_relaxed)) {
    // "S <thread> <pc> <code address on the stack> ..." in one write.
    char line[256];
    size_t n = 0;
    line[n++] = 'S';
    line[n++] = ' ';
    n = AppendHex(line, n, static_cast<uint64_t>(pthread_getthreadid_np()));
    line[n++] = ' ';
    n = AppendHex(line, n, static_cast<uint64_t>(machine->mc_rip));
    const uint64_t* words = reinterpret_cast<const uint64_t*>(machine->mc_rsp);
    int found = 0;
    for (int i = 0; i < 512 && found < 8; ++i) {
      const uint64_t word = words[i];
      if (word > low && word < anchor + 0x20000000) {
        line[n++] = ' ';
        n = AppendHex(line, n, word);
        ++found;
      }
    }
    line[n++] = '\n';
    (void)!write(g_mcla_log_fd, line, n);
    return;
  }
  EarlyHex("  tid ", static_cast<uint64_t>(pthread_getthreadid_np()));
  EarlyHex("  pc ", static_cast<uint64_t>(machine->mc_rip));
  const uint64_t* stack = reinterpret_cast<const uint64_t*>(machine->mc_rsp);
  int printed = 0;
  for (int i = 0; i < 1024 && printed < 14; ++i) {
    const uint64_t word = stack[i];
    if (word > low && word < anchor + 0x20000000) {
      EarlyHex("  stack ", word);
      ++printed;
    }
  }
}

void InstallDumpHandler() {
  struct sigaction action;
  std::memset(&action, 0, sizeof action);
  action.sa_sigaction = DumpHandler;
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&action.sa_mask);
  sigaction(kDumpSignal, &action, nullptr);
}

void Line(const char* format, ...);

void DumpRuntimeThreads(rex::system::KernelState* kernel_state) {
  auto threads = kernel_state->object_table()->GetObjectsByType<rex::system::XThread>();
  Line("THREAD DUMP: %d runtime threads, code anchor %p", static_cast<int>(threads.size()),
       reinterpret_cast<void*>(&EarlyAnchor));
  for (auto& thread : threads) {
    if (!thread || !thread->thread()) continue;
    Line(" thread '%s' id %u%s", thread->name().c_str(), static_cast<unsigned>(thread->thread_id()),
         thread->is_guest_thread() ? " (guest)" : "");
    pthread_kill(reinterpret_cast<pthread_t>(thread->thread()->native_handle()), kDumpSignal);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  }
  Line("THREAD DUMP ends");
}

// A sampling profiler out of the same signal: every runtime thread, `hertz`
// times a second for `seconds`, one line per sample. ps5/profile_report.py
// turns the lines into per-thread function counts on the PC.
void ProfileRuntimeThreads(rex::system::KernelState* kernel_state, int seconds, int hertz) {
  Line("PROFILE begins: %d s at %d Hz, code anchor %p", seconds, hertz,
       reinterpret_cast<void*>(&EarlyAnchor));
  g_profile_sampling.store(true);
  const auto period = std::chrono::microseconds(1000000 / hertz);
  for (int sample = 0; sample < seconds * hertz; ++sample) {
    // Re-read the list now and then: the game creates and ends threads.
    static std::vector<rex::system::object_ref<rex::system::XThread>> threads;
    if (sample % hertz == 0) {
      threads = kernel_state->object_table()->GetObjectsByType<rex::system::XThread>();
    }
    for (auto& thread : threads) {
      if (thread && thread->thread()) {
        pthread_kill(reinterpret_cast<pthread_t>(thread->thread()->native_handle()), kDumpSignal);
      }
    }
    std::this_thread::sleep_for(period);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  g_profile_sampling.store(false);
  Line("PROFILE ends");
}

void Line(const char* format, ...) {
  char text[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof text - 1, format, args);
  va_end(args);
  const size_t length = std::strlen(text);
  text[length] = '\n';
  (void)!write(g_mcla_log_fd, text, length + 1);
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
  // The runtime's log goes to the same place as this file's own lines: the
  // loader socket in a payload, the PC's connection in a title (where
  // standard output cannot be used at all, see ps5/title_log.h).
  rex::LogConfig log_config;
  log_config.log_to_console = false;
  log_config.extra_sinks.push_back(std::make_shared<MclaFdSink>(g_mcla_log_fd));
#ifdef MCLA_LOG_LEVEL
  log_config.default_level = spdlog::level::from_str(MCLA_LOG_LEVEL);
#endif
  log_config.flush_level = spdlog::level::trace;
  rex::InitLogging(log_config);

#if MCLA_STAGE == 5
  // Graphics only: the Xenos GPU system on its Vulkan backend, created from
  // the statically linked plugin, and its provider (instance, device) and
  // presenter. No window, no runtime, no guest code.
  {
    NEXT("create the Xenos graphics system (Vulkan backend)");
    rex::system::GpuCreateInfo create_info;
    create_info.struct_size = sizeof create_info;
    create_info.backend = "vulkan";
    std::unique_ptr<rex::system::IGraphicsSystem> graphics(
        rex_gpu_create(rex::system::kGpuPluginAbiVersion, &create_info));
    if (!graphics) {
      return Finish("the GPU plugin returned no graphics system", 7);
    }
    NEXT("SetupPresentation: Vulkan instance, device and presenter, without a window");
    const auto graphics_status = graphics->SetupPresentation(nullptr);
    if (XFAILED(graphics_status)) {
      Line("SetupPresentation failed: %08X", static_cast<unsigned>(graphics_status));
      return Finish("graphics setup failed", 8);
    }
    Line("PASS Vulkan device and presenter created");
    return Finish("stage 5 complete", 0);
  }
#endif

#if MCLA_STAGE == 6
  {
    NEXT("SDL application context on the offscreen video driver");
    rex::cvar::SetFlagByName("video_driver", "offscreen");
    rex::ui::SDLWindowedAppContext app_context;
    if (!app_context.Initialize()) {
      return Finish("the SDL application context did not initialise", 9);
    }

    NEXT("create the Xenos graphics system (Vulkan backend)");
    rex::system::GpuCreateInfo create_info;
    create_info.struct_size = sizeof create_info;
    create_info.backend = "vulkan";
    std::unique_ptr<rex::system::IGraphicsSystem> graphics(
        rex_gpu_create(rex::system::kGpuPluginAbiVersion, &create_info));
    if (!graphics) {
      return Finish("the GPU plugin returned no graphics system", 7);
    }
    NEXT("SetupPresentation with the application context");
    const auto graphics_status = graphics->SetupPresentation(&app_context);
    if (XFAILED(graphics_status) || !graphics->presenter()) {
      Line("SetupPresentation failed: %08X", static_cast<unsigned>(graphics_status));
      return Finish("graphics setup failed", 8);
    }

    NEXT("create a 1280x720 window");
    auto window = rex::ui::Window::Create(app_context, "mcla", 1280, 720);
    if (!window) {
      return Finish("no window", 10);
    }
    NEXT("open the window");
    if (!window->Open()) {
      return Finish("the window did not open", 11);
    }
    NEXT("attach the presenter to the window (display surface and swapchain)");
    window->SetPresenter(graphics->presenter());
#ifdef MCLA_TITLE
    {
      NEXT("hide the console's launch splash");
      Line("sceSystemServiceHideSplashScreen returned 0x%08X",
           static_cast<unsigned>(sceSystemServiceHideSplashScreen()));
    }
#endif

    NEXT("run the message loop for %d s, requesting a repaint every second", MCLA_RUN_SECONDS);
    std::thread ticker([&app_context, &window]() {
      for (int second = 1; second <= MCLA_RUN_SECONDS; ++second) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        Line("alive: %d s", second);
        app_context.CallInUIThread([&window]() { window->RequestPaint(); });
      }
      app_context.RequestDeferredQuit();
    });
    app_context.RunMainMessageLoop();
    ticker.join();
    Line("PASS message loop ended normally");
    return Finish("stage 6 complete", 0);
  }
#endif

#if MCLA_STAGE >= 7
  // Presentation first, as the desktop host does: the graphics system has to
  // know it will present before the runtime wires it to the guest.
  NEXT("SDL application context on the offscreen video driver");
  rex::cvar::SetFlagByName("video_driver", "offscreen");
  // The SDL input driver looks for an optional controller mapping file by a
  // relative path. In a title that lookup fails with an error other than "not
  // found", which std::filesystem::exists turns into an exception nobody
  // catches. There is no such file here; do not look.
  rex::cvar::SetFlagByName("hid_mappings_file", "");
  // Pacing is the frame limiter's job, as on the desktop (MclaApp::
  // ConfigureFrameTiming): with the emulated console vsync also on, a frame
  // that misses a display interval waits for the next one.
  rex::cvar::SetFlagByName("vsync", "false");
  // A protection change costs 26 us on the console whatever its size (arena
  // step 13), and the GPU emulation was spending a quarter of its thread on
  // them: watch guest physical memory in 64 KiB units, not 16 KiB host pages.
#ifndef MCLA_WATCH_GRANULARITY
#define MCLA_WATCH_GRANULARITY 65536
#endif
#define MCLA_STRINGIZE_(x) #x
#define MCLA_STRINGIZE(x) MCLA_STRINGIZE_(x)
  rex::cvar::SetFlagByName("physical_watch_granularity", MCLA_STRINGIZE(MCLA_WATCH_GRANULARITY));
  rex::ui::SDLWindowedAppContext app_context;
  if (!app_context.Initialize()) {
    return Finish("the SDL application context did not initialise", 9);
  }
  NEXT("create the Xenos graphics system (Vulkan backend)");
  rex::system::GpuCreateInfo gpu_create_info;
  gpu_create_info.struct_size = sizeof gpu_create_info;
  gpu_create_info.backend = "vulkan";
  std::unique_ptr<rex::system::IGraphicsSystem> graphics(
      rex_gpu_create(rex::system::kGpuPluginAbiVersion, &gpu_create_info));
  if (!graphics) {
    return Finish("the GPU plugin returned no graphics system", 7);
  }
  NEXT("SetupPresentation with the application context");
  if (XFAILED(graphics->SetupPresentation(&app_context)) || !graphics->presenter()) {
    return Finish("graphics setup failed", 8);
  }
  NEXT("create and open the window, attach the presenter");
  auto window = rex::ui::Window::Create(app_context, "mcla", 1280, 720);
  if (!window || !window->Open()) {
    return Finish("no window", 10);
  }
  window->SetPresenter(graphics->presenter());
#ifdef MCLA_TITLE
  {
    NEXT("hide the console's launch splash");
    Line("sceSystemServiceHideSplashScreen returned 0x%08X",
         static_cast<unsigned>(sceSystemServiceHideSplashScreen()));
  }
#endif
#endif

  NEXT("construct rex::Runtime");
  auto runtime = std::make_unique<rex::Runtime>(game_root, user_root, std::filesystem::path(),
                                                cache_root, std::filesystem::path());
#if MCLA_STAGE >= 7
  runtime->set_app_context(&app_context);
  runtime->set_display_window(window.get());
#endif

  rex::RuntimeConfig config;
  config.audio_factory = &CreateMclaAudioSystem;
  config.kernel_init = rex::kernel::InitializeKernel;
#if MCLA_STAGE >= 7
  config.graphics = std::move(graphics);
  // The game asks about controllers as soon as it has graphics, and the kernel
  // calls it uses assume an input system exists. The default one, as on the
  // desktop; whether it finds a controller on the console is P7.
#ifdef MCLA_TITLE
  // The console's own pad library (ps5_pad_input.h); SDL has no gamepad
  // backend here. If no controller can be opened, the stand-in driver keeps
  // an idle one present so the game does not wait for a controller forever.
  config.input_factory = [](bool) -> std::unique_ptr<rex::system::IInputSystem> {
    auto input = std::make_unique<rex::input::InputSystem>(nullptr);
    auto pad = std::make_unique<Ps5PadInputDriver>();
    if (pad->Setup() == rex::X_STATUS(0)) {
      input->AddDriver(std::move(pad));
    } else {
      input->AddDriver(std::make_unique<rex::input::nop::NopInputDriver>(nullptr, 0));
    }
    input->SetDeviceAssignment(std::make_unique<rex::input::SlotAssignment>());
    return input;
  };
#else
  config.input_factory = REX_INPUT_BACKEND(rex::input::CreateDefaultInputSystem);
#endif
#else
  // No graphics system and no input system: P5 and P7.
#endif

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
#if MCLA_STAGE >= 7
  if (runtime->input_system()) {
    static_cast<rex::input::InputSystem*>(runtime->input_system())->AttachWindow(window.get());
  }
#endif
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

#if MCLA_STAGE >= 7
  if (runtime->graphics_system()) {
    NEXT("initialise shader storage under %s", cache_root.c_str());
    runtime->graphics_system()->InitializeShaderStorage(cache_root, runtime->kernel_state()->title_id(),
                                                        true);
  }
#endif

  NEXT("resume the main guest thread; guest code runs from here");
  main_thread->Resume();

#if MCLA_STAGE >= 7
  // The message loop needs this thread; a second one reports and ends the run.
  InstallDumpHandler();
  std::thread ticker([&app_context, &runtime]() {
    int next_profile_second = 15;
    for (int second = 1; second <= MCLA_RUN_SECONDS; ++second) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (second <= 30 || second % 30 == 0) Line("alive: %d s", second);
      // Every five seconds: how many protection changes and faults the
      // runtime made, the two costs the profile points at.
      if (second % 5 == 0) {
        static uint64_t last_protects = 0, last_faults = 0;
        const uint64_t protects = rex::memory::Ps5ProtectCallCount();
        const uint64_t faults = rex::arch::Ps5FaultCount();
        Line("STATS %d s: %llu protection changes/s, %llu faults/s", second,
             static_cast<unsigned long long>((protects - last_protects) / 5),
             static_cast<unsigned long long>((faults - last_faults) / 5));
        last_protects = protects;
        last_faults = faults;
      }
      // Twice, a few seconds apart: a thread at the same place both times is
      // stuck there, one that has moved is running.
      if (second == 4 || second == 8) {
        DumpRuntimeThreads(runtime->kernel_state());
      }
#ifdef MCLA_TITLE
      // A profile on request from the controller (L3 + R3 + touchpad), at most
      // one every 40 s: whoever is playing picks the moment.
      if (second >= next_profile_second &&
          g_mcla_profile_request.exchange(false, std::memory_order_relaxed)) {
        Line("PROFILE requested from the controller at %d s", second);
        DumpRuntimeThreads(runtime->kernel_state());
        ProfileRuntimeThreads(runtime->kernel_state(), 20, 25);
        next_profile_second = second + 40;
        g_mcla_profile_request.store(false, std::memory_order_relaxed);
      }
#endif
#ifdef MCLA_PROFILE_AT
      // One profile while the game is being played (the time is chosen at
      // build time and told to whoever holds the controller).
      if (second == MCLA_PROFILE_AT) {
        DumpRuntimeThreads(runtime->kernel_state());
        ProfileRuntimeThreads(runtime->kernel_state(), 20, 25);
      }
#endif
    }
    Finish("run time reached", 0);
  });
  app_context.RunMainMessageLoop();
  ticker.join();
  return Finish("the message loop ended", 0);
#else
  for (int second = 1; second <= MCLA_RUN_SECONDS; ++second) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    Line("alive: %d s", second);
  }
  return Finish("run time reached", 0);
#endif
}
