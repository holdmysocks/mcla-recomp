// Performance overlay: FPS and frame time, plus CPU and RAM on Linux and GPU
// and VRAM with the amdgpu driver. Off by default; the presenter has to
// compose every host frame into the swapchain while an overlay is present.
//
// Toggled with F8 or the PERFORMANCE OVERLAY row in the pause menu, and kept
// in mcla.toml as mcla_perf_overlay. FPS and frame time come from the host-side
// interval between engine timer calls (frame_stats.h): the GPU plugin has its
// own copy of the perf counter registry, so its swap counters are not visible
// here. The load figures are read from the host every half second; on Linux
// they come from /proc and the amdgpu sysfs entries. Other hosts show only FPS
// and frame time.

#pragma once

#include <memory>

#include <rex/cvar.h>

namespace rex::ui {
class ImGuiDialog;
class ImGuiDrawer;
}  // namespace rex::ui

REXCVAR_DECLARE(bool, mcla_perf_overlay);

std::unique_ptr<rex::ui::ImGuiDialog> CreateMclaPerfOverlay(rex::ui::ImGuiDrawer* drawer);
