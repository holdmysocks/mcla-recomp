// Performance overlay: FPS, frame time, CPU, GPU, RAM and VRAM.
//
// Toggled with F8 or the PERFORMANCE OVERLAY row in the pause menu, and kept
// in mcla.toml as mcla_perf_overlay. FPS and frame time come from the host-side
// interval between engine timer calls (frame_stats.h): the GPU plugin has its
// own copy of the perf counter registry, so its swap counters are not visible
// here. The load figures are read from the host every half second; on Linux
// they come from /proc and the amdgpu sysfs entries, and on other hosts the
// system rows show N/A.

#pragma once

#include <memory>

#include <rex/cvar.h>

namespace rex::ui {
class ImGuiDialog;
class ImGuiDrawer;
}  // namespace rex::ui

REXCVAR_DECLARE(bool, mcla_perf_overlay);

std::unique_ptr<rex::ui::ImGuiDialog> CreateMclaPerfOverlay(rex::ui::ImGuiDrawer* drawer);
