# Patches to the ReXGlue SDK

Apply to ReXGlue v0.10.0 (`f5337cdc947ff6d4c4196737e2c807a48f2a1fc2`), from the SDK checkout:

```
git apply ../../patches/rexglue-v0.10.0-mcla.patch
git -C thirdparty/FFmpeg apply ../../../../patches/rexglue-ffmpeg-ps5-config.patch
```

They were separate files at first; they are combined now because several touch the same build files and would not apply independently.

## rexglue-v0.10.0-mcla.patch

| Change | Files | Affects |
|---|---|---|
| Vblank timer resync: the guest tick count can step backwards, which made an unsigned difference wrap and fire vblank interrupts without end | `src/graphics/graphics_system.cpp` | all platforms |
| Per-frame counters kept in Release builds, and their CSV path taken from `REX_PERF_LOG_CSV` (the GPU plugin has its own copy of the logger state, so a path set by the host never reached it) | `CMakeLists.txt`, `src/core/CMakeLists.txt`, `src/system/CMakeLists.txt`, `src/core/perf/counter.cpp` | all platforms |
| Debug-only register lookup removed from every GPU register write in Release builds | `src/graphics/command_processor.cpp` | all platforms |
| PS5 platform: everything else. Platform detection, libc and libc++ differences, anonymous shared memory with a shared-object address reservation at a kernel-chosen base, 16 KiB host pages, signals without realtime signals, no X11/Wayland/RenderDoc, static runtime and GPU libraries, no desktop install rules | the remaining files | PS5 only, behind `REX_PLATFORM_PS5` or `if(PS5)` |

Details of the PS5 changes are in `docs/ps5-port-plan.md`.

## rexglue-ffmpeg-ps5-config.patch

FFmpeg selects a configuration header per platform. This adds `config_ps5_x86_64.h`, generated from the Linux x86-64 one with `HAVE_MALLOC_H`, `CONFIG_ICONV`, `HAVE_MEMALIGN`, `HAVE_SCHED_GETAFFINITY` and `HAVE_LINUX_PERF_EVENT_H` turned off, and selects it for `__PROSPERO__`.
