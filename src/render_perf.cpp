// Rendering performance hooks. See config/render_perf.toml for what each
// address is and where the analysis came from.

#include "generated/default/mcla_init.h"

#include <cstdint>
#include <thread>

#include <rex/cvar.h>
#include <rex/runtime.h>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

REXCVAR_DEFINE_BOOL(mcla_single_tile, true, "MCLA",
                    "Render the main scene as one tile instead of the Xbox 360's two");
REXCVAR_DEFINE_BOOL(mcla_fence_yield, true, "MCLA",
                    "Yield the CPU while the game waits on the GPU instead of spinning");

namespace {

// Tile count the game computed for the current frame.
constexpr uint32_t kGuestTileCount = 0x827D42A4;
// Size of the runtime's virtual EDRAM, in tiles. The console has 0x800.
constexpr uint32_t kHostEdramTiles = 4096;

}  // namespace

void mcla_single_tile(PPCRegister& r7, PPCRegister& r8, PPCRegister& r17, PPCRegister& r25,
                      PPCRegister& r28) {
  if (!REXCVAR_GET(mcla_single_tile)) {
    return;
  }
  // Only the main scene. A non-null render target is an offscreen pass
  // (shadows, bloom, pause snapshot) with its own layout.
  if (r17.u64 != 0) {
    return;
  }
  r7.u64 = r28.u64;
  r8.u64 = r25.u64;
  // The count was stored before this point; the resolve path reads it back.
  uint8_t* count = rex::Runtime::instance()->virtual_membase() + kGuestTileCount;
  count[0] = 0;
  count[1] = 0;
  count[2] = 0;
  count[3] = 1;
}

bool mcla_edram_limit(PPCRegister& r3, PPCRegister& r30, PPCRegister& r11) {
  (void)r30;
  if (!REXCVAR_GET(mcla_single_tile)) {
    return false;
  }
  return r3.u32 < kHostEdramTiles && r11.u32 <= kHostEdramTiles;
}

bool mcla_fence_spin() {
  if (!REXCVAR_GET(mcla_fence_yield)) {
    return false;
  }
  // The caller loops until the GPU thread advances. A short pause keeps the
  // wake-up latency low; every so often give the core away entirely.
  static thread_local uint32_t spins = 0;
  if ((++spins & 0x3F) == 0) {
    std::this_thread::yield();
  } else {
#if defined(_M_X64) || defined(__x86_64__)
    _mm_pause();
#endif
  }
  return true;
}
