// Controller input on PS5: the console's own pad library, presented to the
// runtime as an Xbox 360 controller.
//
// The runtime's input drivers are XInput (Windows) and SDL, and SDL has no
// gamepad backend on the console. This driver opens the first logged-in
// user's controller with scePad and maps it:
//
//   cross, circle, square, triangle -> A, B, X, Y
//   L1, R1 -> shoulders          L2, R2 -> triggers (analogue)
//   L3, R3 -> stick clicks       OPTIONS -> START      touchpad click -> BACK
//
// Only the start of the pad state is read (buttons, sticks, triggers), which
// has the same layout on PS4 and PS5; the buffer handed to scePadReadState is
// larger than either system's structure.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/logging.h>

// The status macros cast to these names unqualified.
using rex::X_RESULT;
using rex::X_STATUS;

extern "C" {
int sceUserServiceInitialize(const void* params);
int sceUserServiceGetInitialUser(int* user_id);
int scePadInit(void);
int scePadOpen(int user_id, int type, int index, const void* param);
int scePadGetHandle(int user_id, int type, int index);
int scePadReadState(int handle, void* data);
int scePadSetVibration(int handle, const void* param);
}

// Set when L3, R3 and the touchpad are all held: the host takes a sampling
// profile of the game as it is at that moment (ps5/profile_report.py).
inline std::atomic<bool> g_mcla_profile_request{false};

class Ps5PadInputDriver final : public rex::input::InputDriver {
 public:
  Ps5PadInputDriver() : InputDriver(nullptr, 0) {}

  rex::X_STATUS Setup() override {
    // Either may already have been done by the system for the title; an
    // "already initialised" result is not a failure.
    const int user_service = sceUserServiceInitialize(nullptr);
    const int pad_init = scePadInit();
    int user_id = -1;
    const int user_result = sceUserServiceGetInitialUser(&user_id);
    handle_ = scePadOpen(user_id, 0 /* standard pad */, 0, nullptr);
    if (handle_ < 0) {
      // Already open (the shell may hold it for the title): ask for the handle.
      handle_ = scePadGetHandle(user_id, 0, 0);
    }
    REXLOG_INFO(
        "PS5 pad: sceUserServiceInitialize 0x{:08X}, scePadInit 0x{:08X}, initial user 0x{:X} "
        "(0x{:08X}), pad handle 0x{:08X}",
        static_cast<uint32_t>(user_service), static_cast<uint32_t>(pad_init),
        static_cast<uint32_t>(user_id), static_cast<uint32_t>(user_result),
        static_cast<uint32_t>(handle_));
    return handle_ >= 0 ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
  }

  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    if (handle_ < 0) return;
    rex::input::DeviceInfo info;
    info.id = kDevice;
    info.name = "PS5 controller";
    out.push_back(info);
  }

  rex::X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                 rex::input::X_INPUT_CAPABILITIES* out_caps) override {
    (void)flags;
    if (id != kDevice || handle_ < 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
      out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
      out_caps->vibration.left_motor_speed = 0xFFFF;
      out_caps->vibration.right_motor_speed = 0xFFFF;
    }
    return X_ERROR_SUCCESS;
  }

  rex::X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out_state) override {
    if (id != kDevice || handle_ < 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    alignas(16) uint8_t data[512];
    std::memset(data, 0, sizeof data);
    const int result = scePadReadState(handle_, data);
    std::lock_guard<std::mutex> lock(mutex_);
    if (result != 0) {
      if (!read_failure_logged_) {
        read_failure_logged_ = true;
        REXLOG_WARN("PS5 pad: scePadReadState failed: 0x{:08X}", static_cast<uint32_t>(result));
      }
      // Keep the device present with an idle state rather than have the game
      // see its controller unplugged.
      if (out_state) {
        std::memset(out_state, 0, sizeof(*out_state));
        out_state->packet_number = packet_number_;
      }
      return X_ERROR_SUCCESS;
    }

    uint32_t pad_buttons;
    std::memcpy(&pad_buttons, data, sizeof pad_buttons);
    const uint8_t left_x = data[4], left_y = data[5], right_x = data[6], right_y = data[7];
    const uint8_t l2 = data[8], r2 = data[9];

    if ((pad_buttons & 0x00100006) == 0x00100006) {
      g_mcla_profile_request.store(true, std::memory_order_relaxed);
    }

    uint16_t buttons = 0;
    const auto map = [&](uint32_t pad_bit, uint16_t xinput_bit) {
      if (pad_buttons & pad_bit) buttons |= xinput_bit;
    };
    map(0x00000010, rex::input::X_INPUT_GAMEPAD_DPAD_UP);
    map(0x00000040, rex::input::X_INPUT_GAMEPAD_DPAD_DOWN);
    map(0x00000080, rex::input::X_INPUT_GAMEPAD_DPAD_LEFT);
    map(0x00000020, rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT);
    map(0x00000008, rex::input::X_INPUT_GAMEPAD_START);          // OPTIONS
    map(0x00100000, rex::input::X_INPUT_GAMEPAD_BACK);           // touchpad click
    map(0x00000002, rex::input::X_INPUT_GAMEPAD_LEFT_THUMB);     // L3
    map(0x00000004, rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB);    // R3
    map(0x00000400, rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER);  // L1
    map(0x00000800, rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER); // R1
    map(0x00004000, rex::input::X_INPUT_GAMEPAD_A);              // cross
    map(0x00002000, rex::input::X_INPUT_GAMEPAD_B);              // circle
    map(0x00008000, rex::input::X_INPUT_GAMEPAD_X);              // square
    map(0x00001000, rex::input::X_INPUT_GAMEPAD_Y);              // triangle

    // Sticks are 0..255 with 128 at rest and y growing downwards; XInput is
    // signed 16-bit with y growing upwards. A small dead zone keeps a resting
    // stick from reading as a constant push.
    const auto axis = [](uint8_t value, bool invert) -> int16_t {
      int centred = static_cast<int>(value) - 128;
      if (centred > -10 && centred < 10) centred = 0;
      int scaled = centred * 258;
      if (invert) scaled = -scaled;
      return static_cast<int16_t>(std::clamp(scaled, -32768, 32767));
    };

    State state;
    state.buttons = buttons;
    state.left_trigger = l2;
    state.right_trigger = r2;
    state.thumb_lx = axis(left_x, false);
    state.thumb_ly = axis(left_y, true);
    state.thumb_rx = axis(right_x, false);
    state.thumb_ry = axis(right_y, true);
    if (std::memcmp(&state, &last_state_, sizeof state) != 0) {
      if (!first_input_logged_ && state.buttons) {
        first_input_logged_ = true;
        REXLOG_INFO("PS5 pad: first button input, XInput buttons 0x{:04X}", state.buttons);
      }
      last_state_ = state;
      ++packet_number_;
    }
    if (out_state) {
      out_state->packet_number = packet_number_;
      out_state->gamepad.buttons = state.buttons;
      out_state->gamepad.left_trigger = state.left_trigger;
      out_state->gamepad.right_trigger = state.right_trigger;
      out_state->gamepad.thumb_lx = state.thumb_lx;
      out_state->gamepad.thumb_ly = state.thumb_ly;
      out_state->gamepad.thumb_rx = state.thumb_rx;
      out_state->gamepad.thumb_ry = state.thumb_ry;
    }
    return X_ERROR_SUCCESS;
  }

  rex::X_RESULT SetDeviceVibration(rex::input::DeviceId id,
                              rex::input::X_INPUT_VIBRATION* vibration) override {
    if (id != kDevice || handle_ < 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (vibration) {
      // { large motor, small motor }, 0..255 each.
      const uint8_t param[2] = {
          static_cast<uint8_t>(static_cast<uint16_t>(vibration->left_motor_speed) >> 8),
          static_cast<uint8_t>(static_cast<uint16_t>(vibration->right_motor_speed) >> 8)};
      scePadSetVibration(handle_, param);
    }
    return X_ERROR_SUCCESS;
  }

  rex::X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                              rex::input::X_INPUT_KEYSTROKE* out_keystroke) override {
    (void)flags;
    (void)out_keystroke;
    if (id != kDevice || handle_ < 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    return X_ERROR_EMPTY;
  }

 private:
  struct State {
    uint16_t buttons = 0;
    uint8_t left_trigger = 0;
    uint8_t right_trigger = 0;
    int16_t thumb_lx = 0;
    int16_t thumb_ly = 0;
    int16_t thumb_rx = 0;
    int16_t thumb_ry = 0;
  };

  static constexpr rex::input::DeviceId kDevice = static_cast<rex::input::DeviceId>(0x50533550);

  int handle_ = -1;
  std::mutex mutex_;
  State last_state_;
  uint32_t packet_number_ = 0;
  bool read_failure_logged_ = false;
  bool first_input_logged_ = false;
};
