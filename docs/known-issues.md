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

## Performance: third play session, `-BigTextureCache` (2026-10-03, run `play-20261003-155942`)

5.4 minutes. User report: a little slow motion, about the same as before.

| Measure | Session 2 (default cache) | Session 3 (1536/2048 MB cache) |
|---|---|---|
| Texture cache misses per minute | 3,445 | 4,176 |
| Frames over 40 ms | 1.7% | 6.2% |
| Slow frames that had texture misses | 510 of 547 | 369 of 580 |
| Frames with more than 5,500 draw calls that were slow | 6% | 30% |

Conclusions:

- **The larger texture cache did not reduce misses**, so the misses are not evictions. They are most likely first-time loads as the open world streams in. The option is not worth its memory and should stay at the default, which also suits the PS5. Not verified: that the three options were actually applied (the log does not echo them).
- **This session is not a clean measurement.** A four-job compile was running in WSL on the same machine for the whole session. The jump in slow heavy-scene frames (6% to 30%) is most likely that competing load, which itself shows the game is CPU-bound in heavy scenes.
- **Slow frames cluster at 50 ms.** With vsync on, a frame that misses the 33.3 ms deadline waits for the next display interval and becomes 50 ms (20 FPS), so a small overrun costs a third of the speed. The median normal frame is already at 33.3 ms with a 95th percentile of 36 ms, so there is little headroom. LARecomp's notes describe the same quantisation and solve it by disabling vsync and pacing frames with a host-side limiter; that needs a frame limiter here, because without one the game runs faster than real time.

## Frame limiter and frame-rate targets (2026-10-03)

`src/frame_timing.cpp` and `config/frame_timing.toml`. The game's timer now receives the real elapsed time instead of a fixed 33.3 ms step, vsync is off by default, and a host limiter sleeps to a wall-clock deadline. `--mcla_fps=30|60|120` selects the target (default 30), `0` is uncapped, `-1` restores the original fixed-step behaviour. Hook addresses and timer analysis are from LARecomp's notes.

Title screen, RTX 4080 and Ryzen 7 7800X3D, D3D12, last 15 seconds of each 60-second run:

| Target | Measured | Median frame | 5th to 95th percentile | Draw calls per frame at that moment |
|---|---|---|---|---|
| 30 | 30.0 FPS | 33.40 ms | 29.6 to 37.1 ms | 6,736 |
| 60 | 60.0 FPS | 16.58 ms | 13.9 to 19.7 ms | 3,873 |
| 90 | 67.5 FPS | 14.55 ms | 13.1 to 17.3 ms | 3,683 |
| 120 | 37.4 FPS | 26.45 ms | 24.4 to 29.8 ms | 7,706 |
| uncapped | 51.3 FPS | 19.45 ms | 17.3 to 22.2 ms | 4,970 |

Reading this:

- **The limiter holds 30 and 60 exactly** when the scene allows it.
- **The game is CPU-bound well below 120 FPS.** The title screen's camera flies over the city, so each run ended on a different view; frame time tracks draw calls at roughly 3.5 to 4 microseconds per draw. The 120 run was not slower because of its target, it ended on a heavier view (7,706 draws). The ceiling on this PC is about 50 to 70 FPS on the title screen.
- **Two threads are saturated** when uncapped (`scripts/thread_profile.ps1`): the guest thread that builds the frame (95% of a core) and the SDK's "GPU Commands" thread that translates Xenos commands to the host API (94%). Everything else is under 10%. Raising the frame rate means speeding up both.
- At 30 FPS a 6,700-draw frame needs about 23 to 26 ms of the 33.3 ms budget, which is why busier gameplay scenes overrun.

Not yet verified:

- Gameplay at any target. Only the title screen was measured. Whether real-delta timing removes the slow motion in races needs a play session.
- Correct game speed at 60. LARecomp reports the simulation is rate-independent with these hooks but needed further fixes for camera smoothing, chassis roll and ground filtering above 30 FPS; none of those are ported here. Intro movies are reported to play fast.
- Frame-to-frame jitter at 30 is about plus or minus 4 ms. The limiter waits inside the timer update, not at present, which may be the cause.
- That `vsync=false` set from the app reaches the GPU plugin. The log reports the value as false and the 60 FPS result is consistent with it.

## Frame-rate correctness fixes (2026-10-03)

User report at `--mcla_fps=60` before these fixes: intro movies play fast; camera smoothing, chassis roll and ground filtering are wrong, as LARecomp's notes predicted.

Ported from LARecomp into `config/frame_timing.toml` and `src/frame_timing.cpp`:

| Hook | Address | What it corrects |
|---|---|---|
| `mcla_camera_pos_smoothing` | 0x82320468 | Chase camera position filter factor, rewritten as a function of elapsed time |
| `mcla_camera_lookat_smoothing` | 0x823204F4 | Chase camera look-at filter factor, same treatment |
| `mcla_chassis_depth_smoothing` | 0x82563720 | Chassis ground-depth filter (0.10 per frame at 30 FPS) |
| `mcla_hook_skip_intro` | 0x822C2F08 | Skips the intro movies (`--mcla_skip_intro`, default on) |
| `mcla_skip_intro_render_pass_mask` | 0x821315E4 | Keeps the skip from enabling an unprepared render pass |

The conversion used for each filter: a per-frame fraction k tuned at 30 FPS becomes `1 - (1 - k)^(dt * 30)` for a step of dt seconds.

Status: builds and boots at the 60 target with no crash. **Not verified by eye**; needs a play session. With the intro skip on, three draws fail once at the moment of the skip (`Resolve region is empty`, six log lines at one timestamp); it does not recur. With the skip off there are none.

## Profile and rendering performance fixes (2026-10-03)

Sampling profile of the title screen, uncapped, before any fix (`--mcla_profile`, `scripts/profile_report.py`, symbol-enabled build):

Game thread (96% busy):

| Share | Where | What it is |
|---|---|---|
| 35.8% | `sub_82412F98` | Fence poll: 32 no-op spin, waiting on the GPU thread |
| 16.4% | `sub_82411E98` | `D3DDevice_BlockOnFence`, the loop around it |
| 13.9% | `sub_823D924C`, `sub_823D91FC` | Register restore/save helpers, called from that loop |
| 2.9% | `sub_8244FEC8` | The game's sleep |

About two thirds of the game thread was waiting. The bottleneck is the SDK's "GPU Commands" thread (90% busy): 62% in the SDK's Xenos translation, 14% in the NVIDIA driver, 10% in ntdll, 7% in memcpy. Within the SDK the largest items were register writes (`GetRegisterInfo` 8.7%, `WriteRegister` 8.5% + 5.5%, `ExecutePacketType0` 3.9%) and `UpdateBindings` 7.3%.

Fixes made:

| Fix | Where | Source |
|---|---|---|
| Single-tile rendering of the main scene, with the EDRAM limit raised to the host's 4096 tiles | `config/render_perf.toml`, `src/render_perf.cpp`, `--mcla_single_tile` | LARecomp |
| Yield in the fence poll instead of spinning | same, `--mcla_fence_yield` | LARecomp |
| Debug-only `GetRegisterInfo` lookup removed from every register write in Release builds | `patches/rexglue-register-write-fastpath.patch` | this project |

Title screen, uncapped, last 15 s of a 60 s run, same build with switches:

| Configuration | FPS | Median frame | Draw calls per frame |
|---|---|---|---|
| Both hooks off | 38.3 | 25.7 ms | 7,967 |
| Fence yield only | 46.3 | 21.5 ms | 6,803 |
| Fence yield and single tile | 64.7 | 15.3 ms | 4,927 |

All three include the register-write patch. Caveat: the title camera moves, and its path depends on elapsed game time, so the three runs are at the same time into the run but not guaranteed to be the identical view. The draw-call drop with single tile is the expected effect of submitting the scene once. A screenshot with both on shows the title screen rendering correctly, with no errors logged. Not yet checked in gameplay or with MSAA-sensitive scenes.

## Intro skip removes the title logo (2026-10-03)

Reported by the user, reproduced with window captures: with `--mcla_skip_intro` the title screen shows only "PRESS START", with or without the render-pass mask change. The skip is now off by default. Consequence: the intro movies play at the limiter's rate, so they are fast at targets above 30. A proper fix is to hold the limiter at 30 while a movie plays; that needs a way to detect movie playback.

Also visible in captures: a dither pattern on foliage (palm fronds, tree shadows). LARecomp lists the same artifact as unresolved.

Screenshots now use `PrintWindow`, which captures only the game window.
