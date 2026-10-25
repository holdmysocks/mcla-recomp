#include "generated/default/mcla_init.h"

#include "mcla_app.h"
#include "perf_overlay.h"

#include <filesystem>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

REXCVAR_DEFINE_STRING(mcla_gpu_backend, "any", "MCLA",
                      "Graphics backend for the Xenos GPU plugin: any, vulkan or d3d12");


std::unique_ptr<rex::system::IAudioSystem> CreateMclaAudioSystem(
    rex::runtime::FunctionDispatcher* function_dispatcher);

// settings_menu.cpp: applies the QUALITY PRESET row's startup options before
// the renderer and the game read them.
void MclaApplyQualityPresetFromConfig();

// Started with no options (a double-click on mcla.exe), the game should just
// run: find the extracted game next to the executable or, as the build leaves
// it, in the repository a few folders up, and load the Xenos GPU plugin.
void MclaApp::OnConfigurePaths(rex::PathConfig& paths) {
  if (!paths.game_data_root.empty()) {
    return;
  }
  std::filesystem::path folder = rex::filesystem::GetExecutableFolder();
  for (int level = 0; level < 5 && !folder.empty(); ++level) {
    std::error_code error;
    if (std::filesystem::exists(folder / "game" / "default.xex", error)) {
      paths.game_data_root = folder / "game";
      return;
    }
    if (folder == folder.parent_path()) {
      break;
    }
    folder = folder.parent_path();
  }
}

void MclaApp::OnPreSetup(rex::RuntimeConfig& config) {
  MclaApplyQualityPresetFromConfig();
  config.audio_factory = &CreateMclaAudioSystem;
  if (config.gpu_plugin.empty()) {
    config.gpu_plugin = "xenos";
  }

  // Left to the SDK when "any"; it then loads the plugin named by --gpu_plugin
  // with its own default backend.
  std::string backend = REXCVAR_GET(mcla_gpu_backend);
  if (backend != "any") {
    config.graphics = rex::system::LoadGpuPlugin("xenos", backend);
    if (!config.graphics) {
      REXLOG_ERROR("could not load the xenos GPU plugin with backend '{}'", backend);
    }
  }
}

void MclaApp::OnPostSetup() {
  ConfigureFrameTiming();


  // The game probes for loose city data on a "t:" drive that retail consoles
  // do not have. Give it a device so the probes fail with "not found" rather
  // than "no such device". LARecomp does the same.
  if (runtime() && runtime()->file_system()) {
    runtime()->file_system()->RegisterSymbolicLink("t:", "\\Device\\Harddisk0\\Partition1");
  }
}

void MclaApp::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  (void)drawer;
  UpdatePerfOverlay();
  // The pause menu changes this from the guest thread; dialogs belong to the
  // UI thread, so apply the change there.
  rex::cvar::RegisterChangeCallback("mcla_perf_overlay",
                                    [this](std::string_view, std::string_view) {
                                      app_context().CallInUIThread(
                                          [this]() { UpdatePerfOverlay(); });
                                    });
  rex::ui::RegisterBind("bind_perf_overlay", "F8", "Toggle performance overlay", [this]() {
    const bool enabled = !REXCVAR_GET(mcla_perf_overlay);
    rex::cvar::SetFlagByName("mcla_perf_overlay", enabled ? "true" : "false");
  });
}

void MclaApp::UpdatePerfOverlay() {
  if (!REXCVAR_GET(mcla_perf_overlay) || !imgui_drawer()) {
    perf_overlay_.reset();
    return;
  }
  if (!perf_overlay_) {
    perf_overlay_ = CreateMclaPerfOverlay(imgui_drawer());
  }
}

void MclaApp::OnShutdown() {
  rex::ui::UnregisterBind("bind_perf_overlay");
  rex::cvar::UnregisterChangeCallbacks("mcla_perf_overlay");
  perf_overlay_.reset();
}
