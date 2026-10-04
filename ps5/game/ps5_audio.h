// Audio output on PS5: the console's own audio library.
//
// The runtime's audio output is SDL, whose only audio driver in this build
// ("dsp") finds no device on the console, so the game ran on the silent
// fallback. The guest submits frames of 256 samples per channel at 48 kHz, six
// channels, big-endian floats, one channel after another. sceAudioOut's main
// port takes 256 samples at 48 kHz as interleaved float stereo, so a frame
// maps to exactly one output call: fold to stereo with the runtime's own
// conversion (the one the SDL driver uses), and hand it over.
//
// sceAudioOutOutput blocks until the previous buffer has been played, which
// is the pacing: one worker thread outputs a frame, or silence if the game
// has none ready, and each frame consumed frees a slot for the guest, as a
// real device would.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <rex/audio/audio_driver.h>
#include <rex/audio/conversion.h>
#include <rex/audio/downmix.h>
#include <rex/logging.h>
#include <rex/thread.h>

extern "C" {
int sceUserServiceInitialize(const void* params);
int sceUserServiceGetInitialUser(int* user_id);
int sceAudioOutInit(void);
int sceAudioOutOpen(int user_id, int type, int index, uint32_t length, uint32_t frequency,
                    uint32_t format);
int sceAudioOutOutput(int handle, const void* data);
int sceAudioOutClose(int handle);
}

class Ps5AudioDriver final : public rex::audio::AudioDriver {
 public:
  Ps5AudioDriver(rex::memory::Memory* memory, rex::thread::Semaphore* semaphore)
      : AudioDriver(memory), semaphore_(semaphore) {}

  ~Ps5AudioDriver() override {
    running_ = false;
    if (worker_.joinable()) {
      worker_.join();
    }
    if (handle_ >= 0) {
      sceAudioOutClose(handle_);
    }
  }

  bool Initialize() {
    sceUserServiceInitialize(nullptr);
    const int init = sceAudioOutInit();
    int user_id = -1;
    sceUserServiceGetInitialUser(&user_id);
    // The main port; float, interleaved stereo.
    constexpr int kPortMain = 0;
    constexpr uint32_t kFormatFloatStereo = 4;
    // The main port belongs to the system user (255), not to a person: opening
    // it with the logged-in user's id returns 0x80260011. That id is tried
    // second in case a firmware wants it the other way.
    constexpr int kSystemUser = 255;
    handle_ = sceAudioOutOpen(kSystemUser, kPortMain, 0, kChannelSamples, 48000, kFormatFloatStereo);
    const int system_user_result = handle_;
    if (handle_ < 0) {
      handle_ = sceAudioOutOpen(user_id, kPortMain, 0, kChannelSamples, 48000, kFormatFloatStereo);
    }
    REXLOG_INFO("PS5 audio: sceAudioOutInit 0x{:08X}; sceAudioOutOpen as the system user 0x{:08X}, "
                "handle in use 0x{:08X}",
                static_cast<uint32_t>(init), static_cast<uint32_t>(system_user_result),
                static_cast<uint32_t>(handle_));
    if (handle_ < 0) {
      return false;
    }
    worker_ = std::thread([this] { Run(); });
    return true;
  }

  void SubmitFrame(uint32_t samples_ptr) override {
    const auto* input = memory_->TranslateVirtual<const float*>(samples_ptr);
    std::vector<float> frame(kFrameSamples);
    std::memcpy(frame.data(), input, kFrameSamples * sizeof(float));
    std::lock_guard<std::mutex> lock(mutex_);
    queued_.push(std::move(frame));
  }

 private:
  static constexpr uint32_t kChannels = 6;
  static constexpr uint32_t kChannelSamples = 256;
  static constexpr uint32_t kFrameSamples = kChannels * kChannelSamples;

  void Run() {
    alignas(32) float output[kChannelSamples * 2];
    bool first_frame_logged = false;
    while (running_) {
      std::vector<float> frame;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!queued_.empty()) {
          frame = std::move(queued_.front());
          queued_.pop();
        }
      }
      if (frame.empty()) {
        std::memset(output, 0, sizeof output);
      } else {
        rex::audio::conversion::sequential_6_BE_to_interleaved_2_LE(
            output, frame.data(), kChannelSamples, rex::audio::GetStereoFold(),
            rex::audio::GetOutputGain());
      }
      const int result = sceAudioOutOutput(handle_, output);
      if (result < 0 && !output_failure_logged_) {
        output_failure_logged_ = true;
        REXLOG_WARN("PS5 audio: sceAudioOutOutput failed: 0x{:08X}", static_cast<uint32_t>(result));
      }
      if (!frame.empty()) {
        if (!first_frame_logged) {
          first_frame_logged = true;
          REXLOG_INFO("PS5 audio: first guest frame played (output returned 0x{:08X})",
                      static_cast<uint32_t>(result));
        }
        semaphore_->Release(1, nullptr);
      } else if (result < 0) {
        // Nothing paced this iteration; do not spin.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
  }

  rex::thread::Semaphore* semaphore_;
  int handle_ = -1;
  std::mutex mutex_;
  std::queue<std::vector<float>> queued_;
  std::atomic<bool> running_{true};
  bool output_failure_logged_ = false;
  std::thread worker_;
};
