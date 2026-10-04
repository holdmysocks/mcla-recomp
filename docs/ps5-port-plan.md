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
| P2 | Guest memory arena runs on the console using the runtime's own code | Title prints the arena base and passes view, alias and protection checks | Passes as a payload (steps 1 to 4); not run as a title |
| P3 | Fault handler works on the console | Title takes a write-watch fault through the runtime's handler and resumes | Unprotect-and-retry passes as a payload (step 5); register-changing cases not run |
| P4 | Runtime links for PS5 with the game code, no graphics | Link succeeds; title starts the guest entry point and logs kernel calls | **Links (2026-10-03):** 52 MB payload, all 311 imports resolve on the console. Not yet run |
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

Checked before any console run: no executable section outside `.text`, no TLS segment (thread-locals use emulated TLS), and `symcheck` reports 0 of 311 imports missing on the console.

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
