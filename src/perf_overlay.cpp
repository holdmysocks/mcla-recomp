// See perf_overlay.h.

#include "perf_overlay.h"

#include "frame_stats.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <imgui.h>

#include <rex/platform.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#if REX_PLATFORM_LINUX
#include <unistd.h>
#endif

// Off by default: a persistent overlay makes the presenter compose every host
// frame into the swapchain instead of presenting the guest output directly,
// which costs performance and is intrusive during normal play.
REXCVAR_DEFINE_BOOL(mcla_perf_overlay, false, "MCLA",
                    "Show the performance overlay (F8): FPS and frame time, plus CPU and RAM "
                    "on Linux, and GPU and VRAM with the amdgpu driver");

namespace {

constexpr double kSampleSeconds = 0.5;
constexpr float kValueColumn = 70.0f;
constexpr size_t kFrameHistorySize = 120;

#if REX_PLATFORM_LINUX

std::string FormatPercent(double percent) {
  if (percent < 0.0) {
    return "N/A";
  }
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%.0f%%", percent);
  return buffer;
}

std::string FormatBytes(uint64_t used, uint64_t total) {
  if (!total) {
    return "N/A";
  }
  constexpr double kGigabyte = 1024.0 * 1024.0 * 1024.0;
  char buffer[64];
  std::snprintf(buffer, sizeof buffer, "%.1f / %.1f GB (%.0f%%)", double(used) / kGigabyte,
                double(total) / kGigabyte, 100.0 * double(used) / double(total));
  return buffer;
}

struct SystemCpuTimes {
  uint64_t idle = 0;
  uint64_t total = 0;
};

bool ReadSystemCpuTimes(SystemCpuTimes& times) {
  std::ifstream file("/proc/stat");
  std::string label;
  uint64_t user = 0;
  uint64_t nice = 0;
  uint64_t system = 0;
  uint64_t idle = 0;
  uint64_t iowait = 0;
  uint64_t irq = 0;
  uint64_t softirq = 0;
  uint64_t steal = 0;
  if (!(file >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal)) {
    return false;
  }
  times.idle = idle + iowait;
  times.total = user + nice + system + idle + iowait + irq + softirq + steal;
  return true;
}

// Total CPU ticks (user + system) of this process, all threads included.
bool ReadProcessCpuTicks(uint64_t& ticks) {
  std::ifstream file("/proc/self/stat");
  std::string line;
  if (!std::getline(file, line)) {
    return false;
  }
  // The executable name is in parentheses and may contain spaces or
  // parentheses itself, so fields are counted from the last ')'.
  const size_t name_end = line.rfind(')');
  if (name_end == std::string::npos || name_end + 2 >= line.size()) {
    return false;
  }
  size_t cursor = name_end + 2;
  // Field 3 is the state; fields 4 to 13 are skipped; 14 and 15 are utime and
  // stime.
  for (int field = 0; field < 11 && cursor < line.size(); ++field) {
    while (cursor < line.size() && line[cursor] != ' ') {
      ++cursor;
    }
    while (cursor < line.size() && line[cursor] == ' ') {
      ++cursor;
    }
  }
  std::istringstream fields(line.substr(cursor));
  uint64_t user = 0;
  uint64_t system = 0;
  if (!(fields >> user >> system)) {
    return false;
  }
  ticks = user + system;
  return true;
}

bool ReadMemInfo(uint64_t& total_bytes, uint64_t& available_bytes) {
  std::ifstream file("/proc/meminfo");
  if (!file) {
    return false;
  }
  bool have_total = false;
  bool have_available = false;
  std::string line;
  while (std::getline(file, line)) {
    unsigned long long kilobytes = 0;
    if (std::sscanf(line.c_str(), "MemTotal: %llu kB", &kilobytes) == 1) {
      total_bytes = uint64_t(kilobytes) * 1024;
      have_total = true;
    } else if (std::sscanf(line.c_str(), "MemAvailable: %llu kB", &kilobytes) == 1) {
      available_bytes = uint64_t(kilobytes) * 1024;
      have_available = true;
    }
    if (have_total && have_available) {
      return true;
    }
  }
  return have_total && have_available;
}

// The amdgpu driver exposes its load and memory use through sysfs. card0 may
// not exist, so the first card with the files is used.
struct GpuSysfsPaths {
  bool scanned = false;
  std::string busy;
  std::string vram_used;
  std::string vram_total;
};

const GpuSysfsPaths& GpuPaths() {
  static GpuSysfsPaths paths;
  if (paths.scanned) {
    return paths;
  }
  paths.scanned = true;
  for (int index = 0; index < 10; ++index) {
    const std::string base = "/sys/class/drm/card" + std::to_string(index) + "/device/";
    if (std::ifstream(base + "gpu_busy_percent").good()) {
      paths.busy = base + "gpu_busy_percent";
      paths.vram_used = base + "mem_info_vram_used";
      paths.vram_total = base + "mem_info_vram_total";
      break;
    }
  }
  return paths;
}

bool ReadUint64File(const std::string& path, uint64_t& value) {
  if (path.empty()) {
    return false;
  }
  std::ifstream file(path);
  return bool(file >> value);
}

#endif  // REX_PLATFORM_LINUX

class MclaPerfOverlayDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit MclaPerfOverlayDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void SampleHostStats(std::chrono::steady_clock::time_point now);

  std::array<float, kFrameHistorySize> frame_history_{};
  size_t frame_history_index_ = 0;
  uint64_t last_frame_count_ = 0;

  std::chrono::steady_clock::time_point last_sample_{};
  bool have_sample_ = false;
  bool have_cpu_baseline_ = false;
  uint64_t last_system_total_ = 0;
  uint64_t last_system_idle_ = 0;
  uint64_t last_process_ticks_ = 0;

  double cpu_percent_ = -1.0;
  double game_cpu_percent_ = -1.0;
  double gpu_percent_ = -1.0;
  uint64_t ram_used_ = 0;
  uint64_t ram_total_ = 0;
  uint64_t vram_used_ = 0;
  uint64_t vram_total_ = 0;
};

void MclaPerfOverlayDialog::SampleHostStats(std::chrono::steady_clock::time_point now) {
  const double elapsed_seconds =
      have_sample_
          ? std::chrono::duration<double>(now - last_sample_).count()
          : 0.0;

#if REX_PLATFORM_LINUX
  SystemCpuTimes system_times;
  const bool have_system_times = ReadSystemCpuTimes(system_times);
  uint64_t process_ticks = 0;
  const bool have_process_ticks = ReadProcessCpuTicks(process_ticks);

  if (have_cpu_baseline_) {
    if (have_system_times && system_times.total > last_system_total_) {
      const uint64_t total_delta = system_times.total - last_system_total_;
      const uint64_t idle_delta = system_times.idle - last_system_idle_;
      cpu_percent_ = 100.0 * double(total_delta - idle_delta) / double(total_delta);
    }
    if (have_process_ticks && process_ticks >= last_process_ticks_ && elapsed_seconds > 0.0) {
      const long ticks_per_second = ::sysconf(_SC_CLK_TCK);
      if (ticks_per_second > 0) {
        game_cpu_percent_ = 100.0 * double(process_ticks - last_process_ticks_) /
                            (double(ticks_per_second) * elapsed_seconds);
      }
    }
  }

  if (have_system_times) {
    last_system_total_ = system_times.total;
    last_system_idle_ = system_times.idle;
  }
  if (have_process_ticks) {
    last_process_ticks_ = process_ticks;
  }
  if (have_system_times && have_process_ticks) {
    have_cpu_baseline_ = true;
  }

  uint64_t memory_total = 0;
  uint64_t memory_available = 0;
  if (ReadMemInfo(memory_total, memory_available)) {
    ram_total_ = memory_total;
    ram_used_ = memory_total - std::min(memory_available, memory_total);
  }

  const GpuSysfsPaths& paths = GpuPaths();
  uint64_t value = 0;
  if (ReadUint64File(paths.busy, value)) {
    gpu_percent_ = double(value);
  }
  if (ReadUint64File(paths.vram_used, value)) {
    vram_used_ = value;
  }
  if (ReadUint64File(paths.vram_total, value)) {
    vram_total_ = value;
  }
#else
  (void)elapsed_seconds;
#endif

  have_sample_ = true;
}

void MclaPerfOverlayDialog::OnDraw(ImGuiIO& io) {
  const auto now = std::chrono::steady_clock::now();
  if (!have_sample_ ||
      std::chrono::duration<double>(now - last_sample_).count() >= kSampleSeconds) {
    SampleHostStats(now);
    last_sample_ = now;
  }

  // The engine timer hook measures the real game frame interval. The overlay
  // itself may repaint more often (or less often) than the game presents, so
  // io.DeltaTime is not a substitute for this.
  const MclaFrameStats frame_stats = MclaGetFrameStats();
  const bool have_frame_time = frame_stats.frame_count > 0 && frame_stats.smoothed_frame_time_us > 0;
  const double smoothed_frame_ms =
      have_frame_time ? double(frame_stats.smoothed_frame_time_us) / 1000.0 : 0.0;
  if (frame_stats.frame_count != last_frame_count_) {
    last_frame_count_ = frame_stats.frame_count;
    if (frame_stats.frame_time_us > 0) {
      frame_history_[frame_history_index_] = float(frame_stats.frame_time_us) / 1000.0f;
      frame_history_index_ = (frame_history_index_ + 1) % kFrameHistorySize;
    }
  }

  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12.0f, 12.0f), ImGuiCond_FirstUseEver,
                          ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.55f);
  if (ImGui::Begin("##mcla_perf", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs)) {
    auto row = [](const char* label, const std::string& value) {
      ImGui::TextDisabled("%s", label);
      ImGui::SameLine(kValueColumn);
      ImGui::TextUnformatted(value.c_str());
    };

    char buffer[64];
    if (have_frame_time) {
      std::snprintf(buffer, sizeof buffer, "%.1f", 1000.0 / smoothed_frame_ms);
      row("FPS", buffer);
      std::snprintf(buffer, sizeof buffer, "%.1f ms", smoothed_frame_ms);
      row("FRAME", buffer);
    } else {
      row("FPS", "N/A");
      row("FRAME", "N/A");
    }

    ImGui::PlotLines("##frame_history", frame_history_.data(), int(frame_history_.size()),
                     int(frame_history_index_), nullptr, 0.0f, 50.0f, ImVec2(180.0f, 34.0f));

#if REX_PLATFORM_LINUX
    row("CPU", FormatPercent(cpu_percent_));
    row("MCLA", FormatPercent(game_cpu_percent_));
    // The GPU and VRAM figures come from sysfs, which only the amdgpu driver
    // exposes. Hide them rather than showing two permanent N/A rows on other
    // GPUs; the CPU and memory figures above still work.
    if (!GpuPaths().busy.empty()) {
      row("GPU", FormatPercent(gpu_percent_));
    }
    row("RAM", FormatBytes(ram_used_, ram_total_));
    if (vram_total_) {
      row("VRAM", FormatBytes(vram_used_, vram_total_));
    }
#endif
  }
  ImGui::End();
}

}  // namespace

std::unique_ptr<rex::ui::ImGuiDialog> CreateMclaPerfOverlay(rex::ui::ImGuiDrawer* drawer) {
  return std::make_unique<MclaPerfOverlayDialog>(drawer);
}
