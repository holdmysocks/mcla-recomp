// Controller button prompts: Xbox or PlayStation glyphs.
//
// Both glyph sets ship in the game's own UI files; this only sets the
// `platform` value the UI scripts read. See config/ui.toml. Mechanism from
// LARecomp.

#include "generated/default/mcla_init.h"

#include "mcla_app.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_STRING(mcla_button_prompts, "auto", "MCLA",
                      "Button glyphs: auto (match the controller), xbox or playstation");

namespace {

constexpr uint32_t kImageBegin = 0x82000000;
constexpr uint32_t kSwfSetVarInt = 0x825EE0E0;   // (context, name, int)
constexpr uint32_t kStringPlatform = 0x8201ABDC;  // the game's own "platform" string

std::atomic<uint32_t> g_platform_var{0};
std::atomic<int> g_applied{-1};
std::atomic<int> g_detected{0};

#if defined(_WIN32)
// True when a Sony controller (USB vendor 054C) is attached.
bool PlayStationControllerPresent() {
  UINT count = 0;
  if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0) {
    return false;
  }
  std::vector<RAWINPUTDEVICELIST> devices(count);
  if (GetRawInputDeviceList(devices.data(), &count, sizeof(RAWINPUTDEVICELIST)) == UINT(-1)) {
    return false;
  }
  for (UINT i = 0; i < count; ++i) {
    if (devices[i].dwType != RIM_TYPEHID) {
      continue;
    }
    char name[512];
    UINT size = sizeof name;
    if (GetRawInputDeviceInfoA(devices[i].hDevice, RIDI_DEVICENAME, name, &size) == UINT(-1)) {
      continue;
    }
    std::string upper(name);
    for (char& c : upper) {
      c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }
    // USB: VID_054C. Bluetooth: VID&0002054C.
    if (upper.find("VID_054C") != std::string::npos ||
        upper.find("VID&0002054C") != std::string::npos) {
      return true;
    }
  }
  return false;
}
#endif

// 1 = PlayStation glyphs, 0 = Xbox glyphs.
int WantedPlatform() {
  std::string mode = REXCVAR_GET(mcla_button_prompts);
  if (mode == "playstation") {
    return 1;
  }
  if (mode == "xbox") {
    return 0;
  }
#if defined(_WIN32)
  return g_detected.load(std::memory_order_relaxed);
#elif REX_PLATFORM_PS5
  return 1;
#else
  return 0;
#endif
}

void WritePlatformVar(uint32_t entry, int value) {
  uint8_t* base = rex::Runtime::instance()->virtual_membase();
  // +0x58: the integer value. +0x5C: its "%d" text.
  base[entry + 0x58] = 0;
  base[entry + 0x59] = 0;
  base[entry + 0x5A] = 0;
  base[entry + 0x5B] = static_cast<uint8_t>(value);
  base[entry + 0x5C] = static_cast<uint8_t>('0' + value);
  base[entry + 0x5D] = 0;
}

struct MovieContext {
  uint32_t context = 0;
  int value = -1;
};
constexpr int kMaxMovies = 16;
MovieContext g_movies[kMaxMovies];
int g_movie_count = 0;

}  // namespace

// Called once per frame from the frame-timing hook.
void MclaTickButtonPrompts() {
#if defined(_WIN32)
  // Re-detect about every two seconds so plugging in a controller is noticed.
  static auto next_check = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  if (now >= next_check) {
    next_check = now + std::chrono::seconds(2);
    g_detected.store(PlayStationControllerPresent() ? 1 : 0, std::memory_order_relaxed);
  }
#endif
  const uint32_t entry = g_platform_var.load(std::memory_order_relaxed);
  if (!entry) {
    return;
  }
  const int want = WantedPlatform();
  if (want == g_applied.load(std::memory_order_relaxed)) {
    return;
  }
  WritePlatformVar(entry, want);
  g_applied.store(want, std::memory_order_relaxed);
  REXLOG_INFO("button prompts: {}", want ? "PlayStation" : "Xbox");
}

void mcla_platform_var_init(PPCRegister& r27) {
  const uint32_t entry = r27.u32;
  if (entry < 0x10000) {
    return;
  }
#if defined(_WIN32)
  g_detected.store(PlayStationControllerPresent() ? 1 : 0, std::memory_order_relaxed);
#endif
  const int want = WantedPlatform();
  g_platform_var.store(entry, std::memory_order_relaxed);
  WritePlatformVar(entry, want);
  g_applied.store(want, std::memory_order_relaxed);
  REXLOG_INFO("button prompts: {} (platform variable at 0x{:08X})", want ? "PlayStation" : "Xbox",
              entry);
}

bool mcla_platform_push(PPCRegister& r5) {
  r5.u64 = static_cast<uint64_t>(WantedPlatform());
  return true;
}

void mcla_swf_context_enter(PPCRegister& r3) {
  const uint32_t context = r3.u32;
  if (!context || !g_platform_var.load(std::memory_order_relaxed)) {
    return;
  }
  const int want = WantedPlatform();
  int slot = -1;
  for (int i = 0; i < g_movie_count; ++i) {
    if (g_movies[i].context != context) {
      continue;
    }
    if (g_movies[i].value == want) {
      return;
    }
    slot = i;
    break;
  }
  if (slot < 0) {
    slot = g_movie_count < kMaxMovies ? g_movie_count++ : 0;
    g_movies[slot].context = context;
  }
  // Write only into the context being run right now; nothing is kept to be
  // written later, so a destroyed movie can never be touched.
  if (PPCFunc* set_var = rex::Runtime::instance()->function_dispatcher()->GetFunction(kSwfSetVarInt)) {
    rex::ppc::GuestToHostFunction<uint32_t>(set_var, context, kStringPlatform, uint32_t(want));
  }
  g_movies[slot].value = want;
}
