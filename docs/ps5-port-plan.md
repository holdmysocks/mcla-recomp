# PS5 platform layer: plan and progress

Started 2026-10-03, after the five feasibility probes in `ps5-feasibility.md` all passed.

## Shape of the port

The recompiled game code and `src/` are shared with PC unchanged. What changes is the ReXGlue runtime's host layer and how the pieces are linked:

- **Host:** Arch Linux (WSL), cross-compiling with the driver author's fork of the PS5 payload SDK (clang with the PS5 target, the SDK's libc++, and its `libps5platform`). Ubuntu's LLVM 18 cannot link PS5 titles against the driver.
- **Linking:** one static executable. The Vulkan driver is a static archive linked whole, and the runtime's GPU plugin, which is a DLL on PC, has to be linked in rather than loaded.
- **SDK changes** are kept in `patches/` (see its README) against ReXGlue v0.10.0; the PS5 ones are all behind `REX_PLATFORM_PS5` or `if(PS5)`.

## Milestones

| # | Milestone | Verified by | Status |
|---|---|---|---|
| P1 | Runtime builds for PS5 | Build log | **Done 2026-10-03.** `librexruntime.a` (22.9 MB) and `librexgpu-xenos.a` (7.7 MB) built with the runtime's own CMake: 184 steps, 0 failures. Not linked into a title yet |
| P2 | Guest memory arena runs on the console using the runtime's own code | Title prints the arena base and passes view, alias and protection checks | **Done 2026-10-03**, as a payload (shared object) and as a title (direct memory) |
| P3 | Fault handler works on the console | Title takes a write-watch fault through the runtime's handler and resumes | **Done 2026-10-03**, as a payload and as a title: retry, skip and emulated load. XMM write-back untested |
| P4 | Runtime links for PS5 with the game code, no graphics | Link succeeds; title starts the guest entry point and logs kernel calls | **Done 2026-10-03, as a payload.** Guest code runs on the console: kernel imports resolved, volume calls made, and the game reached video setup (`VdInitializeRingBuffer`), where it stalls for lack of a graphics system as expected. 20 s run, no crash |
| P5 | Presentation: the Vulkan backend creates its device and swapchain | A frame is presented | **Done 2026-10-04.** Device, `VK_KHR_display` surface, swapchain; frames presented and visible once the shell's launch splash is hidden |
| P6 | Boots to the title screen | Screenshot or user report | **Done 2026-10-04 (user report):** intro movies, then the title screen, as an installed title. About 14 presents a second once the game is drawing; picture size and placement not yet checked against the 4K mode |
| P7 | Audio output and controller input | User report | Not started |
| P8 | Performance on the console | Per-frame counters read back | Not started |

## Changes made so far (P1)

All in the runtime's POSIX layer, each behind `REX_PLATFORM_PS5`:

| Area | Change | Why |
|---|---|---|
| Platform detection | `REX_PLATFORM_PS5` for `__PROSPERO__`/`__FreeBSD__` | The runtime only knew Windows, Linux, Android and macOS |
| C++ library gaps | Reuse the macOS fallbacks for floating-point `from_chars` and `clock_cast`; build with `-fexperimental-library` for `std::jthread` | The SDK's libc++ is older |
| Large-file calls | Plain `fseeko`, `ftello`, `ftruncate`, `mmap`, `fstat` | FreeBSD-derived libc has no `*64` variants; `off_t` is already 64-bit |
| Clock | `CLOCK_MONOTONIC` in place of `CLOCK_MONOTONIC_RAW` | Not defined; 81 ns resolution measured |
| Thread id and names | `pthread_getthreadid_np`, `pthread_set_name_np`; names are write-only | No `gettid` or `pthread_getname_np` |
| Thread affinity | Reported as "all processors", setting is a no-op | FreeBSD's `cpuset_t` API differs; left for later tuning |
| Thread signalling | `SIGUSR1`/`SIGUSR2` with `pthread_kill`, as on macOS | No `pthread_sigqueue` |
| Shared memory object | `shm_open(SHM_ANON)`, nothing to unlink | Probe 1 |
| Arena reservation | Map the shared object `PROT_NONE` at a kernel-chosen address, then fixed views inside it (the macOS code path with a different reservation call) | Probe 1: anonymous reservations above about 2 GiB fail, and the executable sits at 0x200000000 |
| Commit | `mprotect` inside the reservation, as on macOS | No `MAP_FIXED_NOREPLACE` semantics to rely on |
| 16 KiB pages | The 0xE0000000 guest range uses the same 0x1000 host offset as on Apple Silicon | Probe 1: page size 16384. This constant is compiled into the generated game code, so that code must be built with the PS5 definition too |

## Known gaps, to be dealt with at the milestone named

- **P3:** the machine context is 0x40 bytes into the signal context on firmware 13.42 (probe 2). The two payload SDKs declare it differently (v0.43: 0x10; mihawk-99's fork: 0x40), so `exception_handler_ps5.cpp` uses the fixed offset and not the header's. Other firmware versions unknown.
- **P2/P3:** `QueryProtect` has no implementation without `/proc` or Mach calls, so the "previous protection" a caller asks for is reported as no-access. Callers that depend on it need checking.
- **P2:** unnamed POSIX semaphores (`sem_init`) are used for thread suspend; untested on the console.
- **P4:** the GPU plugin is loaded by file name; the Vulkan loader, RenderDoc and SPIRV-Tools are opened by library name. All need static equivalents.
- **P4:** SDL is used for windowing, audio and input on the other platforms and has no PS5 backend here.
- **P8:** write-watch faults cost about 105 microseconds each on the console (probe 1).

## Building the runtime for PS5

On the Arch host, with the driver's SDK fork set up by `ps5/build_ps5_vulkan_driver.sh`:

```
SDK=/root/ps5vk/PS5_Vulkan/.deps/native/ps5-payload-sdk
cmake -S rexglue-sdk -B build-ps5 -G Ninja   -DCMAKE_TOOLCHAIN_FILE=$SDK/toolchain/prospero.cmake   -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=23   -DCMAKE_C_FLAGS="-march=znver2" -DCMAKE_CXX_FLAGS="-march=znver2 -fexperimental-library"   -DREXGLUE_USE_D3D12=OFF -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_TRACY=OFF
ninja -C build-ps5 rexruntime rexgpu-xenos
```

The source tree needs its git metadata present (the build checks submodules and derives its version from tags).

How the build got there, for the record: the first full attempt had 172 failed steps from five causes (FFmpeg had no PS5 configuration; RenderDoc's header only knows Windows and Linux; window code included X11; the Linux surface source was always built; one kernel file lacked a socket header). After those, the only failures were the shared-library link wanting `librt`, and the desktop install rules objecting to a static runtime.

Stubbed, not solved, at this point:

- **No window or surface on PS5.** The window code compiles with a PS5 branch that returns no surface. Presentation through `VK_KHR_display` is milestone P5.
- **SDL is built with no video or audio backend.** Audio and input need PS5 implementations (P7).
- **FFmpeg uses a configuration derived from Linux.** Compiles; behaviour unverified.
- **Thread affinity is a no-op** and thread names are write-only.

## Linking against the runtime: two things the payload SDK does not handle

Found with the single-step payloads (`ps5/probes/arena`), where a payload linked with the runtime printed nothing at all while a plain one worked.

1. **Large code model sections.** The runtime is compiled with the large code model, so nearly all of its code is in `.ltext.*` sections (2,337 of them in the test payload) and its data in `.lrodata`/`.ldata`/`.lbss`. The SDK's linker script (`target/lib/main.script`) names only `.text`, `.rodata`, `.data` and `.bss`. The others become orphans, and the linker put the code after `.dynamic`, in the read/write segment that is not executable. The payload died on its first call. libc++'s `__lcxx_override` section (`operator new`) has the same problem once a script is supplied.
   Fix: link with a script derived from the SDK's that lists those sections with their normal counterparts (`ps5/probes/arena/build.sh` generates it and fails the build if any executable section is outside `.text`). **The game link (P4) needs the same script.**
2. **Imports are checked only on the console.** The SDK's startup code resolves every import before any payload code runs and on a miss aborts with a message that goes only to the kernel log. Its stub libraries are generated lists, so a clean link proves nothing. `ps5/probes/symcheck` builds a print-only payload that looks up another payload's imports with `dlsym` on the console and prints the missing ones. For the arena payload: 0 missing of 144.

A constructor with priority 101 that installs a crash reporter (signal number, fault address, instruction pointer) and writes straight to the socket is what made these visible; it is worth keeping in every console test.

## P4: the game as a payload, without graphics

`ps5/game/build.sh` builds it on the Arch host: the 121 recompiled sources (with their precompiled header), the five host sources that the recompiled code calls into for hooks (`audio_fallback`, `frame_timing`, `render_perf`, `button_prompts`, `settings_menu`), and `ps5/game/main_ps5.cpp`, linked with the runtime's static libraries using the linker script from `ps5/payload_ld.sh`.

- **No ReXApp.** The desktop host is built around a window and an event loop. `main_ps5.cpp` drives `rex::Runtime` directly in the same order: `Setup`, `LoadXexImage`, guest heap, `PrepareModuleLaunch`, resume. No graphics system and no input system are supplied; audio uses the existing silent fallback.
- **Stages.** `-DMCLA_STAGE=1..4` stops after setup, XEX load, main-thread creation, or a timed run of guest code, so the first console runs can go one at a time. Every stage is announced on the loader socket before it starts.
- **Game data** is read from `/data/mcla/game` on the console (the user's own extracted copy; nothing from it is in the repository).
- **Floating point.** The game code is built with `-ffp-contract=off`: the desktop build targets SSE4.1 and never fuses a multiply and an add, and with `-march=znver2` the compiler otherwise would.
- **Fibers.** The runtime's fiber support had no PS5 source. `fiber_ps5.cpp` implements thread fibers only (every guest thread converts itself to one); `Create` and `SwitchTo` are not implemented, and nothing linked into this game references them.
- **Not built for PS5:** `mcla_app.cpp`/`main.cpp` (the windowed host) and the development tools tied to it (code-pointer scan, crash trace, sampling profiler). Their settings (frame timing configuration, the `t:` link aside) are therefore at defaults.

### Console runs (2026-10-03)

| Stage | Result |
|---|---|
| 1 `Runtime::Setup` | Pass. Arena at 0x202E74000; function table for 82130000-827CD054 |
| 2 `LoadXexImage` | Pass. Title id 545407F8 |
| 3 `PrepareModuleLaunch` | Pass. The game does not use the runtime's guest heap |
| 4 resume, 20 s | Pass. The main guest thread looked up the `XInputdFF*` exports (not implemented, same as on the desktop), called `IoDismountVolumeByFileHandle` twice, then `VdSetGraphicsInterruptCallback`, `VdInitializeRingBuffer` and `VdEnableRingBufferRPtrWriteBack`, each ignored because no GPU plugin is loaded. Nothing further was logged; the process stayed alive until the timer ended it |

Game data was uploaded to `/data/mcla/game` over FTP (13 files, 6.2 GB, sizes verified). Not tested: anything past video setup, more than one guest thread doing real work, write-watch faults under load, running as a title.

### The same as an installed title (2026-10-03)

`TITLE=<id> ps5/game/build.sh <stage>` links the same objects with the Vulkan driver through `ps5/title_build.sh` (a 77 MB executable). Three more libc functions had no definition in the title link (`getresuid`, `getresgid`, `timegm`) and are in `ps5/title_support.c` with the six null imports. The host sends its own lines and the runtime's log to the title log connection.

| Stage | Result as a title |
|---|---|
| 1 `Runtime::Setup` | Pass. Arena on direct memory at 0x200020000. `/data/mcla/game` is readable from inside the title |
| 4 XEX load, main thread, 20 s of guest code | Pass. Same sequence as the payload run: volume calls, then `VdSetGraphicsInterruptCallback`, `VdInitializeRingBuffer` and `VdEnableRingBufferRPtrWriteBack` ignored for lack of a GPU system; alive to the end |

So the game runs as a title up to the point where it needs graphics. Of the title's 418 imports, four were not covered by the console import check (`fork`, `vfork`, `setsid`, `_Unwind_Backtrace`).

Checked before any console run: no executable section outside `.text`, no TLS segment (thread-locals use emulated TLS), and `symcheck` reports 0 of 311 imports missing on the console.

## P5: graphics, in stages

The Vulkan driver only works in an installed title, and a title cannot load a module at run time, so everything is linked in and found without a loader.

Runtime changes (in the SDK patch):

- **Vulkan bootstrap** (`vulkan_instance.cpp`): on PS5 there is no loader library. The driver is an ICD linked into the executable, so the instance code takes `vk_icdGetInstanceProcAddr` (a weak reference; a build without the driver, such as a payload, links and reports "no Vulkan driver") and gets every other function from it.
- **Surface type** (`surface.h`, `surface_ps5.h`, `vulkan_presenter.cpp`, `window_sdl.cpp`): `kTypeIndex_Ps5Display`, supported when the instance has `VK_KHR_display`. The presenter creates the Vulkan surface with `vkCreateDisplayPlaneSurfaceKHR` on the first display and plane, using the mode whose visible region is the surface size (1920x1080 for now; the guest renders at 720p and the presenter scales), and logs every display and mode the driver reports. SDL is built with its offscreen video driver, so the existing SDL window can stand for the display.
- **GPU plugin**: linked statically; the host calls its factory `rex_gpu_create` directly instead of loading `rexgpu-xenos` by name.

Known loose end from stage 6: the surface reports 1920x1080 but the only display mode on the test console (a 4K television) is 3840x2160, so the swapchain is 4K while the presenter believes the surface is 1080p. The surface should take its size from the display mode; on a 1080p display the mode list will differ and has not been seen.

Host stages (`ps5/game/main_ps5.cpp`):

| Stage | What it does | Result |
|---|---|---|
| 5 | Create the Xenos graphics system on Vulkan and call `SetupPresentation` with no window: instance, device, presenter | **Pass on the console.** Instance API 1.4.354 with `VK_KHR_display`, `VK_KHR_surface`, `VK_EXT_debug_utils`; device `PlayStation 5 GPU (RADV NAVI21)` with `VK_KHR_swapchain`, `VK_EXT_custom_border_color`, `VK_EXT_fragment_shader_interlock`, `VK_EXT_memory_budget`, `VK_EXT_non_seamless_cube_map`, `VK_EXT_robustness2`, `VK_EXT_shader_stencil_export`; presenter created |
| 6 | A window on SDL's offscreen video driver standing for the display; attaching the presenter makes the `VK_KHR_display` surface and the swapchain; the message loop runs with a repaint requested every second. No guest code | **Pass on the console.** SDL video driver `offscreen`. One display, `PS5 VideoOut`, 3840x2160, with one mode: 3840x2160 at 59.94 Hz. One plane. Surface created; swapchain 3840x2160, format 44 (`B8G8R8A8_UNORM`), present mode 2 (FIFO). The message loop ran its 10 s and ended normally |
| 7 | The game with graphics: the window and graphics system of stage 6 handed to the runtime, shader storage under `/data/mcla/cache`, then the guest runs with the message loop on the main thread | Built as a title; not run |

The stage 5 title imports eight functions the console import check did not cover: `__pthread_cleanup_pop_imp`, `__pthread_cleanup_push_imp`, `nanf`, `pthread_setcancelstate`, and the four from before (`fork`, `vfork`, `setsid`, `_Unwind_Backtrace`). A null one would show as a jump to address 0 in the crash report.

### Stage 7 findings (2026-10-03/04)

- **First run:** 30 s, no crash, but the screen kept the console's own loading background (no frame flipped) and the log went silent right after `SetInterruptCallback`. Debug logging showed no further kernel calls.
- **Thread dump** (added to the host: `SIGXCPU` to each runtime thread, handler prints pc and stack code addresses, symbolised on the PC with `out/symdump.py`): the main guest thread was in the game's GPU fence wait (`sub_82412F98` via `mcla_fence_spin`), the GPU command thread idle waiting for commands, the vsync thread delivering interrupts. The game had submitted work the GPU never saw.
- **Not the cause:** protection on direct memory. Arena step 12 (title): a write faults after plain `mprotect` and after `sceKernelMprotect`, to no access and to read-only (5 of 5).
- **A real bug found:** the generated header `mcla_pch.h` has its own copy of the 0xE0000000 physical range's 4 KiB host offset rule (`REX_PHYS_HOST_OFFSET`), naming only Windows and Apple Silicon. The runtime applies the offset on PS5; the game code did not, so the two disagreed by 4 KiB about that memory, where the game builds its GPU command buffers. Fixed in the SDK's codegen templates (`pch_h.inja`, `ppc_config_h.inja`) and in the generated header; `ps5/game/build.sh` now fails if the header lacks it, and rebuilds the hook sources when the header changes.
- **The rebuilt title is 94 MB** (`.text` 60.8 MB against 51 MB). It was uploaded and verified byte for byte. Before it was launched the console went into rest mode on its own and had to be brought back; that was not caused by the title, which had not been started.

- **Stage 7 with the offset fix (2026-10-04):** the game gets further. It runs past graphics setup (the button-prompt hook logs `PlayStation`, as the desktop build does at that point), and then two guest threads fail to start: `CreateThread failed`, `C0000017` (no memory). 21 runtime threads existed at the dump. The run completed its 20 s without a crash.
- **Why threads fail:** each guest thread asks for a 16 MiB host stack. In a title a stack pthread allocates comes out of flexible memory (448 MiB in all), which a couple of dozen threads exhaust. Fix in the runtime (`threading_posix.cpp`, PS5 only): the stack is one direct-memory allocation mapped above a reserved guard range and handed to `pthread_attr_setstack`. Stacks are pooled and reused two seconds after their thread object is destroyed, not released, since there is no point at which the thread is known to have left its stack for good. The direct-memory functions are weak references, so a payload keeps default stacks. Not yet run.

- **Stage 7 with direct-memory thread stacks:** no thread creation failures. The GPU emulation received the game's commands for the first time and created its first graphics pipelines on the console's driver (VS `1E6883FCCDE1F688` / PS `A4A965C189287B99`, VS `88F617431F7D9C9B` / PS `46BE7CEEBA3ECD76`). Then the title was gone 0.25 s after the game started, with nothing from the host's crash reporter.
- **Kernel log** (`ps5/probes/klog`, a print-only payload that streams `/dev/klog` to the loader socket; the backlog is not kept, so it must be running during the run): "A user thread receives a fatal signal", `SIGSEGV` on `Main XThread`, with registers and a backtrace. Symbolised with `ps5/symdump.py`: `XamInputGetCapabilities_entry`, called from the game. The host had given the runtime no input system, and that kernel call dereferences it. Fixed in the host: the runtime's default input system, attached to the window, as the desktop host does. (The kernel's stated reason was "FPU device not available"; the fault itself is a null dereference.)
- A title's fatal signal went to the system's crash handling (core dump, then the title is killed) and not to the host's reporter, although the reporter does catch faults in the arena title. Something installs a handler after it in the game title; not looked into.

- **With the input system:** died in start-up. Kernel log and the host's reporter both: `std::filesystem::exists("gamecontrollerdb.txt")` in `SDLInputDriver::OnWindowAvailable` throws inside a title (the relative lookup fails with an error other than "not found"), nothing catches it, `abort`. The host now sets `hid_mappings_file` empty. SDL's gamepad subsystem itself initialised.
- **Stage 7, 2026-10-04 00:24: the game runs for the full 30 s as a title with graphics.** The log matches the desktop build's start-up line for line: graphics interrupt callback, button prompts (`PlayStation`), first pipelines, the `t:\mc4\art\city\*.loc` probes failing as they should, audio falling back to silent (`SDL_INIT_AUDIO` has no device: P7), and the same "texture fetch constant has invalid type" warning the desktop build logs a few seconds in. 38 then 40 runtime threads. In both thread dumps the GPU command thread was inside real work (`VulkanSharedMemory::UploadRanges`, `radv_UpdateDescriptorSets`, pipeline barriers) and the main guest thread in a kernel wait. What reached the screen is not known from the log.
- At the timed end of the run the host's `_exit` path reported signal 12 from a system library; this is the deliberate hard stop with 40 threads running, not something the game did.

- **Why nothing was on screen.** With PS5-only logging in `VulkanPresenter::PaintAndPresentImpl` (first six paints, then one in 300): paints run, every `vkAcquireNextImageKHR` and `vkQueuePresentKHR` returns `VK_SUCCESS`, images cycle 0, 1, 2. So frames were being flipped all along, under the console shell's launch splash, which covers a title until it calls `sceSystemServiceHideSplashScreen`. The host now calls it after attaching the presenter. Two things about linking it: the SDK's stub for `libSceSystemService` does not list the function, so `ps5/title_build.sh` adds a link stub (`ps5/title_stub_system_service.c`); and it must be a plain import, because the title converter leaves a weak import unbound (the first attempt was weak, resolved to null, and was silently skipped).
- **2026-10-04 01:07: `sceSystemServiceHideSplashScreen` returned 0**, and the run completed its 45 s. Present rate from the log: about 57 paints a second for the first five seconds, then about 14 a second once the game starts drawing (paint 300 at +5 s, 600 at +26 s, 900 at +51 s).

- **P6 reached, 2026-10-04 01:07 (user report): the game loads and reaches its title screen on the console's display.** The intro movies play too fast, the same pacing issue the desktop build has above 30 FPS. No button did anything: there was no controller backend.
- **Controller (P7, first half).** SDL has no gamepad backend on the console. `ps5/game/ps5_pad_input.h` is an input driver on the console's own pad library (`sceUserServiceGetInitialUser`, `scePadOpen`, `scePadReadState`), mapped to an Xbox 360 controller: cross/circle/square/triangle to A/B/X/Y, L1/R1 to the shoulders, L2/R2 to the analogue triggers, L3/R3 to the stick clicks, OPTIONS to START, the touchpad click to BACK, with stick y inverted and a small dead zone. Only the first ten bytes of the pad state are read (buttons, sticks, triggers). The title host builds its input system from this driver alone, with the stand-in driver if no pad opens. Not yet run.

## P7/P8: in-game on the console (2026-10-04)

**Input works.** `ps5/game/ps5_pad_input.h` on the console's pad library; the user played with it: menus, career, driving in the city. Full screen, correct proportions on a 4K display.

**No audio yet.** SDL's only audio driver here is `dsp`, which finds no device; the silent fallback runs. An audio output driver on the console's own audio library is still to write.

**Performance is the open problem: 8 to 17 presents a second, choppy, physics feel wrong.** What has been measured:

| Measurement | Result |
|---|---|
| Sampling profile while driving (host profiler, `ps5/profile_report.py`), three sessions | 36 of 40 runtime threads idle. GPU command thread: about 40% blocked in `std::recursive_mutex::lock`, about 20% in `rex::memory::Protect` under `PhysicalHeap::EnableAccessCallbacks` from `SharedMemory::RequestRanges`, the rest rendering. Main guest thread: about 35% in the GPU fence wait, 25% in game code |
| Arena step 13, timings in a title | `mprotect`: 26 us a call, the same for anonymous and direct memory, 1 page or 64, fragmented or not. Protect + faulting write + handler + retry: 57 us. `std::recursive_mutex` lock+unlock: 0.013 us alone, 2.9 us with four threads in a tight loop |
| Host counters, every 5 s | About 6,500 protection changes and 900 faults a second, steady from the menus to driving |

Reading: 6,500 protection changes at 26 us are 17% of one thread, real but not the 40% the GPU thread spends blocked. The profile's "on the stack" figures include stale stack words and are not reliable; the "most often in" figures are.

Changes tried:

| Change | Effect |
|---|---|
| Emulated console vsync off (as the desktop host does) | None felt |
| `physical_watch_granularity` = 64 KiB (new runtime setting; write-watch units of 64 KiB, not the 16 KiB host page) | Confirmed active; no change in the present rate or in how it felt. Kept as a setting |
| Global critical region spins before sleeping on PS5 (`AcquireGlobalLockSpinning` in `rex/thread/mutex.h`) | **Helped.** The GPU thread's 43% blocked in `recursive_mutex::lock` is gone (5% in `try_lock`); protection changes and faults a second roughly doubled (12,000 and 1,700), presents rose from 8-11 to 12-14 a second and later to 19-30. User: slightly better frame rate. A thread that slept on that lock was costing far more to wake than the wait was worth; the tight-loop test in step 13 could not show it |

**Protection changes by caller** (host counters, after the lock change): about 7,000 a second enabling write-watches, about 2,300 from watches being triggered, 1,300 faults; none from host-page reconciliation or stale-protection recovery.

**The clock was 81 times fast (found 2026-10-04 from the user's "fast forward" report).** `Clock::host_tick_frequency_platform` derived the tick frequency from `clock_getres`, which is right only where the resolution is 1 ns. On the console it is 81 ns, while the tick count is in nanoseconds, so guest time ran 81 times too fast. The game's per-frame clamp of 0.125 s hid it: at 8 frames a second the clamp gives exactly real time, at 14 it gives 1.75x, at 30 it gives 3.75x, which is what appeared once the lock change raised the frame rate. It also explains the fast intro movies and the odd physics (every frame stepping by the maximum), and the emulated 60 Hz vblank and every timed wait were firing 81 times too often. Fixed in `clock_posix.cpp`: on PS5 the frequency is 1e9. Run 2026-10-04 with the fix, 4.5 minutes in game: a steady 16 to 17 presents a second (300 every 18 s), about 15,600 protection changes a second (11,500 enabling watches, 4,000 from triggers) and 1,800 faults. That is about 930 protection changes and 110 faults a frame, and at 26 us each the protection changes alone are 0.4 s of work a second. The frame limiter's target is 30. The figures before this line were taken with the wrong clock.

The title's name and art: `ps5/make_title_art.py` reads the dashboard art from the user's own `nxeart` (an STFS package) and writes a background and a tile (the corner of the background that carries the game's logo) to an ignored folder; `ps5/title_build.sh` converts them when `ART_DIR` is set. The shell caches a title's name and art at registration, so the tile has to be deleted and re-registered to show a change.

## Console crash on the first P2/P3 run (2026-10-03)

The first title that ran the runtime's own code on the console, `PPSA99778` ("MCLA Arena Test": `Memory::Initialize`, guest heaps, physical mirrors, a 256 MiB commit and three deliberate faults through the new PS5 fault handler), **crashed the whole console**, not just the title. The user had to re-jailbreak.

What is known:

- The title was launched from the home screen and the console went down.
- No results file exists in the title's folder afterwards. The file was created with `fopen` and flushed with `fflush` after each line, which does not survive a kernel crash; nothing was synced to disk. So the failing step is unknown.
- The earlier probes ran the same kinds of operations in a simpler form without trouble: a 4.5 GiB shared-object reservation with two fixed views, page protection changes, and a caught-and-resumed fault. Those ran as payloads through the ELF loader; this ran as an installed title.

What differs from the probes, and is therefore suspect, in no established order:

| Suspect | Why |
|---|---|
| The runtime maps about a dozen fixed views at different offsets, not two | More, and different, `MAP_FIXED` mappings over the reservation |
| Reserve and release paths in the memory layer | On PS5 a "reserve" with a fixed address maps anonymous memory over part of a shared view, and release unmaps part of it; decommit also calls `madvise(MADV_DONTNEED)`. None of that was in the probes |
| The fault handler writes registers back into the signal context | Probe 2 only read the context. If the layout differs for a title compared with a payload, the write lands in the wrong place, and a corrupted context handed back to the kernel is a plausible way to take the kernel down. Fault test 1 writes back unchanged values; tests 2 and 3 write changed ones |
| Running as a title, not a payload | Different process setup; the 0x30 context shift was measured in a payload only |
| The Vulkan driver archive is linked whole into the title | Not initialised by the test, but its static constructors run |

### What the crashed title actually was (found 2026-10-03, at the start of P5)

The crashed title's linked ELF was still on the build host (`build/mcla-arena/llvm-pie.elf`). Its layout has the same fault as the first payloads: the driver project's linker script (`tooling/native/ps5-pie.ld`) names only `.text`, so `main` and all 2,835 `.ltext.*` sections of the runtime were placed at 0x17E7190 and up, inside the read/write segment (0x16D4000 to 0x18F3542), which is not executable.

So that title cannot have run any of its tests. It would have faulted on the first call from the startup code into `main`, which is consistent with there being no log at all. The suspects in the table above (fixed views, reserve and release, the context write-back, the driver's constructors) were never reached, and each has since passed as a payload.

Why an immediate fault in a title took the whole console down, rather than just ending the title, is **not** known. A title built with the corrected script (`ps5/title_build.sh`, which also fails the build if any executable section is outside `.text`) does not take the console down:

| Title run | Result |
|---|---|
| `PPSA99779`, arena step 0 (startup only), first build | Connected to the log receiver and exited; no text arrived. Console unaffected |
| Same, log written to the socket directly | Pass: all lines received. Console unaffected |
| Step 8: the three fault cases through the runtime's handler | Pass, 12 of 12. The signal context is at the same offset in a title. The title image is at 0x400000 (a payload's is at 0x200000000) |

Two things learned about titles from these runs:

- **`dup2` onto descriptors 1 and 2 fails with `EPERM` in a title.** Standard output cannot be redirected to a socket, so a title's log has to be written to the connection itself (`g_mcla_log_fd` in `ps5/title_log.h`). The runtime's log will need a sink of its own for the game title.
- A title can listen on a TCP port and accept a connection from the PC before `main` runs.

Step 9 (the memory steps together) **fails as a title**: the process dies inside `Memory::Initialize` with a jump to address 0, caught by the crash reporter; the console is unaffected. A jump to 0 is a call through an import that no module available to a title exports (a payload is given a different kernel library, which is why the same code passes there). `ps5/probes/symcheck/title_main.cpp` is a print-only title that lists every such import. Its first run, on the payload builds' 311 imports, found none null. The backtrace from a second step 9 run (after fixing the crash reporter's stack scan for a title's low load address) named the call: `spdlog::details::os::in_terminal` calls `isatty`, from the coloured console sink that the runtime's first log line creates. `isatty` is not in a payload's dynamic import list, so the check has to be built from the title's own imports (433 names with the game's). The memory code itself has still not run as a title.

Run over the title's own imports, the check found six null functions: `isatty`, `link`, `mkstemp`, `pathconf`, `readlink`, `symlink`. `ps5/title_support.c` supplies replacements and `ps5/title_build.sh` binds them. With those, step 9 reaches `Memory::Initialize`, which **returns failure** (no crash; console unaffected). The runtime's message giving the reason went to standard output and was lost, so two things were added: a log sink that writes to the title log connection (`ps5/log_fd_sink.h`), and step 10, which makes the arena's raw system calls one at a time with error codes, for `shm_open(SHM_ANON)`, a named `shm_open` and (title only) `memfd_create`. Step 10 passes as a payload, 16 of 16.

**Step 10 as a title gives the reason.** All three ways of creating the object succeed, and all three fail at the same call: `ftruncate` to 0x120010000 returns `ENOMEM`. A title has a memory budget that a payload process does not, and a 4.5 GiB shared object is over it. So in a title the arena cannot be backed the way it is in a payload. Step 11 (title only) asks what can be used instead: how large a shared object a title may have, and whether direct memory can do the job (`sceKernelReserveVirtualRange` for the range, one `sceKernelAllocateDirectMemory` allocation mapped at two addresses with `sceKernelMapDirectMemory`, protection changes with both `sceKernelMprotect` and plain `mprotect`, and finally one 4.5 GiB allocation mapped whole).

**Step 11 as a title: pass, 15 of 15.** What it measured on firmware 13.42:

| Question | Answer |
|---|---|
| Direct memory | 12,288 MiB total, 12,270 MiB free in one range |
| Flexible memory (anonymous mappings, libc, shared objects) | 448 MiB configured, 418 MiB available |
| Largest shared object | 256 MiB sizes; 1 GiB is refused. It comes out of the flexible budget |
| Reserve 4.5 GiB of address space (`sceKernelReserveVirtualRange`) | Works; placed at 0x200020000 |
| One direct allocation mapped at two addresses | Works; the views are the same memory |
| Protection changes on a direct-memory view | `sceKernelMprotect` and plain `mprotect` both work, including to no access |
| Mapping direct memory with no access | Works |
| 4.5 GiB of direct memory in one allocation, mapped whole | Works; start, middle and end usable |

**Resulting change to the runtime** (`memory_posix.cpp`, `xmemory.cpp`, `rex/memory/utils.h`): `CreateFileMappingHandle` still tries the shared object first, and when it cannot be sized falls back to one direct-memory allocation of the same length. `MapFileView`, `UnmapFileView` and the new `ReserveFileMappingRange` / `ReleaseFileMappingRange` do the right thing for either backing, and the arena's reservation and views now go through them. The direct-memory functions are weak references, so a build whose kernel library lacks them still links. A payload takes the shared-object path exactly as before (the memory steps re-run as a payload after the change: 35 of 35).

Costs and open points of the direct-memory backing:

- It is **eager**: 4.5 GiB of the title's 12 GiB is taken at start, whether the guest uses it or not. A shared object is committed page by page. Enough for now; the unused guest ranges could be left unbacked later.
- Decommit still calls `madvise(MADV_DONTNEED)`, whose effect on direct memory is unknown. If the guest relies on decommitted pages reading as zero when recommitted, that needs handling.
- **Step 9 as a title with this backing: pass, 35 of 35** (`Memory::Initialize`, the four virtual heaps, the physical heap through all four views, protection, the system heap, the 256 MiB commit, teardown). Arena at 0x200020000 on direct offset 0x2400000.

Rules adopted for further console runs:

1. Every log line leaves the console over the network before the step it describes runs. No reliance on files on the console.
2. One new risky operation per run, least risky first.
3. The user is told beforehand that the run can crash the console and agrees to that run specifically.
4. Where the same code can run as a payload through the ELF loader, do that first: its output arrives live.

### Single-step payload runs (2026-10-03, after the crash)

`ps5/probes/arena` builds one payload per step (`-DMCLA_STEP=N`), each printing a `NEXT ...` line to the loader socket before every operation.

| Step | What it runs | Result |
|---|---|---|
| 0 | Startup only: constructors, `main`, exit | Pass, after the two link fixes above |
| 5 | Runtime fault handler: write to a read-only page, handler unprotects, registers written back unchanged | Pass, 5 of 5, after the context-offset fix |
| 6 | Handler skips the faulting instruction (changes `rip`) | Pass, 3 of 3 |
| 7 | Handler emulates a load (changes `rip` and a register) | Pass, 4 of 4 |
| 1 | `Memory::Initialize` and teardown: shared object, 4.5 GiB reservation, fixed views, heaps | Pass, 2 of 2. Virtual base 0x20025C000 (directly after the payload image), physical base 4 GiB above it |
| 2 | Virtual heaps at 0x00010000, 0x40000000, 0x80000000, 0x90000000: allocate 1 MiB, fill, read back, release | Pass, 18 of 18 |
| 3 | Physical heap: 1 MiB seen through the 0xA0000000, 0xC0000000, 0xE0000000 (+0x1000) and raw physical views; protect read-only and back; release | Pass, 13 of 13 |
| 4 | System heap allocate/free; 256 MiB allocated in the 0x40000000 heap, touched in 32 MiB pieces, verified, released | Pass, 8 of 8 |

One thing this established about the crashed title: it was built with the same SDK fork, so its fault handler read and wrote the context 0x30 bytes past the right place (the first step-5 run stopped on exactly that, through a guard that refuses to write back when the instruction pointer it read is not inside the faulting function). Whether that is what took the console down is **not** established; the title's memory steps ran before its fault tests and have not been re-run.

With step 4, every operation the crashed title performed has run cleanly as a payload. What the title had that these runs did not: the wrong context offset in its fault handler, an installed-title process rather than a payload, and the Vulkan driver archive linked in. Which of those took the console down is not established.

P2 is verified as a payload (steps 1 to 4). P3 is verified as a payload for all three cases (retry, skip, emulated load); XMM register write-back and running as a title are not tested.
