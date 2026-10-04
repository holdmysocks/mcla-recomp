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
| P2 | Guest memory arena runs on the console using the runtime's own code | Title prints the arena base and passes view, alias and protection checks | Not started |
| P3 | Fault handler works on the console | Title takes a write-watch fault through the runtime's handler and resumes | Not started |
| P4 | Runtime links for PS5 with the game code, no graphics | Link succeeds; title starts the guest entry point and logs kernel calls | Not started |
| P5 | Presentation: the Vulkan backend creates its device and swapchain | A frame is presented | Not started |
| P6 | Boots to the title screen | Screenshot or user report | Not started |
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

- **P3:** the fault handler reads registers through the SDK's `ucontext_t`, which is 0x30 bytes off on firmware 13.42 (probe 2). Needs a corrected context read.
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
- **Thread affinity is a no-op**, thread names are write-only, and the fault handler still reads registers through the SDK's context structure (P3).
