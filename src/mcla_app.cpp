#include "generated/default/mcla_init.h"

#include "mcla_app.h"

#include <filesystem>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>

REXCVAR_DEFINE_STRING(mcla_gpu_backend, "any", "MCLA",
                      "Graphics backend for the Xenos GPU plugin: any, vulkan or d3d12");


std::unique_ptr<rex::system::IAudioSystem> CreateMclaAudioSystem(
    rex::runtime::FunctionDispatcher* function_dispatcher);

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
