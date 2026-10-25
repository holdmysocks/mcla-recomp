// Host-side frame timing shared between the frame-timing hooks and overlays.
//
// The GPU plugin carries its own copy of the SDK's perf counters, so the
// plugin's swap counters are not visible to the host overlay. The engine timer
// hook is called once per rendered frame on the game thread, so measuring the
// interval between those calls gives the real game frame rate.

#pragma once

#include <cstdint>

struct MclaFrameStats {
  uint64_t frame_count = 0;
  uint32_t frame_time_us = 0;
  uint32_t smoothed_frame_time_us = 0;
};

MclaFrameStats MclaGetFrameStats();
