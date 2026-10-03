# Known issues and findings

Updated 2026-10-03. PC (Windows, D3D12 and Vulkan). On PS5 only the memory probe has run; see `ps5-feasibility.md`.

## Milestone reached

**Boots to the title screen** on Windows/D3D12 (2026-10-03, run 20261003-103007): the Complete Edition logo over the live 3D city with traffic, "PRESS START" prompt, about 38 s after launch. The process was still running at 75 s with 77 threads and 1.36 GB working set, and the log had no errors other than the missing audio device.

**Menus, races and the garage work** on Windows/D3D12 (2026-10-03, user play session with an Xbox 360 controller, about 20 minutes across five launches). The user reported: gameplay very good, audio fine, got into the garage, installed hydraulics and they worked. Four crashes on unrecovered functions, each fixed by the recovery loop and not seen again.

Problems the user reported in that session:

- **Frame-rate drops on loading screens.**
- **Random "slow motion" during races.** The game runs a fixed timestep, so any stretch below 30 FPS plays slow. Cause not yet established. Candidates: the SDK vblank-timer underflow that LARecomp documented (fixed here in `patches/rexglue-vblank-resync.patch`, effect unverified), pipeline/shader compilation hitches, and write-watch faults.

To find out, Release builds now keep the SDK's per-frame counters (`patches/rexglue-perf-csv-env.patch`). Set `REX_PERF_LOG_CSV=<file>` before launching and run `scripts/analyze_perf.py <file>` afterwards.

## Boot blockers found so far

| # | Symptom | Cause | Status |
|---|---|---|---|
| 1 | `Validation failed: N unresolved calls` at codegen | Tail-call targets not inside any discovered function | Fixed with 8 hints in `config/mcla.toml` |
| 2 | `Call to invalid or unregistered function at guest address ...` at run time | Functions reached only through pointers | Ongoing. 30 found by data scan (`config/data_referenced.toml`), the rest one per run (`config/runtime_discovered.toml`, `scripts/recover_loop.ps1`) |
| 3 | `VFS: 't:\mc4\art\city' -> [no device]` | Game probes a `t:` drive for loose city files | Fixed: `t:` is mapped to the game root in `MclaApp::OnPostSetup`. The probes now fail with "not found", which the game tolerates |
| 4 | Guest null-pointer read at 0x00002EF0 in `sub_821C8FE0` | See below | Fixed with the silent audio driver in `src/audio_fallback.cpp` |

### Issue 4 in detail

The game's audio engine init (`sub_82144B90`) calls the statically linked XAudio library (`sub_823EAA88`), which registers a render-driver client with the kernel. If the host cannot create an audio driver, that returns 0xC0000002 or a failure code, `sub_82144B90` returns false, the caller (`sub_82144EB0`) skips creating the engine's handle table (pointer at guest 0x828369C4), and the first user of that table dereferences null.

Two host conditions trigger it:

- SDL cannot open an output device (`No default audio device available`). This PC is in that state.
- The SDK's `NopAudioSystem`, whose `CreateDriver` returns `X_STATUS_NOT_IMPLEMENTED`.

So the game needs an audio driver to exist even when there is nowhere to play sound. `SilentAudioDriver` accepts frames, discards them, and releases the client semaphore once per 256-sample frame period (5.33 ms) so the guest runs at real-time pace. This matters for PS5 bring-up too: the game can boot before the PS5 audio output is written.

Useful addresses learned along the way:

| Address | What |
|---|---|
| 0x82144B90 | Audio engine init, nine sequential checks |
| 0x82135E48 | Loads `$/audio/X360/Config/driverSettings.xml`, then sets up voice pools |
| 0x8213AB78 | Voice pool setup: 100 voices x 8320 bytes, 2048-byte alignment |
| 0x823EAA88 | XAudio library init (XAUD static lib) |
| 0x82160800 | Creates the 800-slot handle table |

## Open risks, not yet investigated

- 20 `Unexpected float16_4 pack instruction` codegen warnings in 0x8243C67C to 0x8243D158.
- Vulkan reaches the same title screen (run `20261003-103756`), but only with the default feature set on an RTX 4080. The minimal feature set is not established.
- Audio output itself is untested because this PC has no default output device.
- Only boot and the title screen have been seen. Menus, loading a race and gameplay are untested (they need controller or keyboard input).
- Frame rate and frame pacing have not been measured.

## Tooling notes

- `--mcla_scan_code_pointers=<file>` scans the loaded image for data words pointing at unrecovered function entries and exits.
- Guest crash tracer (`src/crash_trace.cpp`): on a null-page fault, logs the host stack with each frame mapped to its guest function. Windows only for now; needs a `sigaction` version for Linux and PS5.
- Git on Windows checks out the SDK's libmspack symlinks as text files. They must be replaced with copies of their targets before building the SDK.
- lldb from the portable LLVM package does not start (needs `python310.dll`).

## Performance: second play session (2026-10-03, run `play-20261003-153451`)

18.5 minutes in one launch, D3D12, with the vblank resync patch and per-frame counters. No crashes. User report: two small hitches, much better than the first session, frame-rate drops still present on loading screens.

| Measure | Value |
|---|---|
| Frames | 32,603 |
| Frame time | median 33.3 ms, p95 36.1 ms, p99 50.2 ms, max 437.5 ms |
| Frames over 40 ms | 547 (1.7%), 37 s in total (3% of the session) |
| Longest slow stretch | 4.0 s |
| Interrupts per frame | about 5 normally, 6 in slow frames: no interrupt storm |
| Pipeline cache misses | 0 in both normal and slow frames |

What the slow frames have in common: **texture cache misses**. 510 of the 547 slow frames (35 of the 37 slow seconds) had at least one miss; the median slow frame had 38, the median normal frame 0. Misses are steady through the whole session at 2,500 to 5,400 per minute, 63,484 in total. Most frames with misses are still on time (8,658 frames had misses, 510 of those were slow), so it is the bursts that hurt.

Not established:

- Whether the misses are first-time loads (streaming new areas) or reloads after eviction. The steady rate over 18 minutes in one city suggests eviction. LARecomp reports that raising the cache limits to 1536 MB soft / 2048 MB hard / 64 MB render-to-texture took misses to zero. `scripts/play_loop.ps1 -BigTextureCache` applies those values; not yet tested here.
- Whether the improvement over the first session came from the vblank patch. The first session had no counters, so there is no before/after measurement. No storm occurred in this session, which is consistent with the patch working but does not prove it.
- Which slow stretches were loading screens. The counters carry no game-state marker.

PS5 note: a 2 GB texture cache is a memory-budget question there, and the budget is still unmeasured.
